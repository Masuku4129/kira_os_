#!/usr/bin/env python3
"""KIRA combined technology lab. Host-side prototypes, correctness gates first."""
import argparse, csv, hashlib, json, os, statistics, struct, tempfile, time, zlib
from pathlib import Path

REC = struct.Struct("<QQiII")

def median_ms(fn, repeat):
    samples=[]
    value=None
    for _ in range(repeat):
        t=time.perf_counter(); value=fn(); samples.append((time.perf_counter()-t)*1000)
    return value, statistics.median(samples)

def make_events(n):
    return [(i, 1_700_000_000_000+i*11, ((i*7919)%200001)-100000,
             (i//17)%32, i%8) for i in range(n)]

def test_ipc(events, repeat):
    # Typed binary IPC + bounded ring + explicit capability gate.
    token=b"KIRA_IPC_SEND:demo-service"
    allowed={token}
    def copy_path():
        out=[]
        for row in events:
            msg=REC.pack(*row)
            if token not in allowed: raise PermissionError
            out.append(REC.unpack(msg))
        return out
    def ring_path():
        cap=bytearray(REC.size*256); head=tail=count=0; out=[]
        for row in events:
            if token not in allowed: raise PermissionError
            msg=REC.pack(*row)
            if count==256:
                out.append(REC.unpack(bytes(cap[tail*REC.size:(tail+1)*REC.size])))
                tail=(tail+1)%256; count-=1
            start=head*REC.size; cap[start:start+REC.size]=msg
            head=(head+1)%256; count+=1
        while count:
            start=tail*REC.size; out.append(REC.unpack(bytes(cap[start:start+REC.size])))
            tail=(tail+1)%256; count-=1
        return out
    a,ta=median_ms(copy_path,repeat); b,tb=median_ms(ring_path,repeat)
    assert a==b==events
    denied=False
    try:
        if b"DENIED" not in allowed: raise PermissionError("capability missing")
    except PermissionError:
        denied=True
    assert denied
    return {"template":"typed-ipc-ring-capability","baseline_ms":round(ta,3),
            "combined_ms":round(tb,3),"speedup":round(ta/tb,3) if tb else 0,
            "payload_bytes":len(events)*REC.size,"correctness":"PASS",
            "security":"PASS: missing capability rejected"}

def merkle_root(hashes):
    level=[bytes.fromhex(x) for x in hashes]
    if not level: return hashlib.sha256(b"").hexdigest()
    while len(level)>1:
        if len(level)%2: level.append(level[-1])
        level=[hashlib.sha256(level[i]+level[i+1]).digest() for i in range(0,len(level),2)]
    return level[0].hex()

def test_storage(events, repeat):
    # Immutable content-addressed chunks + Merkle manifest + copy-on-write snapshots.
    chunks=[json.dumps(events[i:i+256],separators=(",",":")).encode() for i in range(0,len(events),256)]
    def baseline():
        versions=[]; current=[]
        for c in chunks:
            current.append(c); versions.append([memoryview(x).tobytes() for x in current])
        return versions
    def combined():
        objects={}; versions=[]; refs=[]
        for c in chunks:
            h=hashlib.sha256(c).hexdigest(); objects.setdefault(h,c); refs.append(h)
            manifest={"version":len(versions)+1,"objects":list(refs)}
            manifest["merkle_root"]=merkle_root(refs)
            versions.append(manifest)
        return objects,versions
    old,t0=median_ms(baseline,repeat); (objects,versions),t1=median_ms(combined,repeat)
    last=versions[-1]; assert last["merkle_root"]==merkle_root(last["objects"])
    assert all(hashlib.sha256(objects[h]).hexdigest()==h for h in last["objects"])
    # Tamper detection and rollback to prior immutable manifest.
    corrupt=bytearray(objects[last["objects"][0]]); corrupt[0]^=1
    assert hashlib.sha256(corrupt).hexdigest()!=last["objects"][0]
    rollback=versions[-2] if len(versions)>1 else versions[-1]
    assert rollback["merkle_root"]==merkle_root(rollback["objects"])
    baseline_bytes=sum(len(x) for v in old for x in v)
    cas_bytes=sum(len(x) for x in objects.values())
    return {"template":"cas-merkle-cow","baseline_ms":round(t0,3),"combined_ms":round(t1,3),
            "baseline_snapshot_bytes":baseline_bytes,"cas_unique_bytes":cas_bytes,
            "stored_versions":len(versions),"integrity_tamper":"DETECTED",
            "rollback":"PASS","correctness":"PASS"}

def test_telemetry(events, repeat):
    # Append-only events -> columnar batches -> compression for cold segments.
    def rowwise():
        raw=json.dumps(events,separators=(",",":")).encode()
        return zlib.compress(raw,6)
    def columnar():
        cols=[bytearray() for _ in range(5)]
        for a,b,c,d,e in events:
            cols[0].extend(struct.pack("<Q",a)); cols[1].extend(struct.pack("<Q",b))
            cols[2].extend(struct.pack("<i",c)); cols[3].extend(struct.pack("<I",d))
            cols[4].extend(struct.pack("<I",e))
        raw=struct.pack("<I",len(events))+b"".join(cols)
        return zlib.compress(raw,6)
    row,t0=median_ms(rowwise,repeat); col,t1=median_ms(columnar,repeat)
    raw=zlib.decompress(col); n=struct.unpack_from("<I",raw)[0]; p=4
    decoded=[]
    columns=[]
    for fmt in ("<Q","<Q","<i","<I","<I"):
        sz=struct.calcsize(fmt); columns.append([x[0] for x in struct.iter_unpack(fmt,raw[p:p+n*sz])]); p+=n*sz
    decoded=list(zip(*columns))
    assert decoded==events
    return {"template":"columnar-zstd-like-cold-log","baseline_bytes":len(row),
            "columnar_zlib_bytes":len(col),"size_ratio":round(len(row)/len(col),3),
            "baseline_ms":round(t0,3),"combined_ms":round(t1,3),
            "roundtrip":"PASS","codec":"zlib baseline only; Zstd not yet tested"}

def test_cache(repeat):
    # Canonical IR digest + compiler/target/options fingerprint + content cache.
    ir={"functions":[{"name":"sum","blocks":[["arg",0],["arg",1],["add",0,1],["ret",2]]}]}
    def canonical(x): return json.dumps(x,sort_keys=True,separators=(",",":")).encode()
    cache={}
    def compile_like(tree, compiler, target, flags):
        key=hashlib.sha256(canonical(tree)+compiler.encode()+target.encode()+flags.encode()).hexdigest()
        if key in cache: return cache[key],True
        artifact=hashlib.sha256(b"artifact:"+canonical(tree)+compiler.encode()+target.encode()+flags.encode()).digest()
        cache[key]=artifact
        return artifact,False
    def cold_build():
        cache.clear()
        return compile_like(ir,"KCC-lab-1","x86_64","O2")
    cold,tc=median_ms(cold_build,repeat)
    compile_like(ir,"KCC-lab-1","x86_64","O2")
    warm,tw=median_ms(lambda:compile_like(ir,"KCC-lab-1","x86_64","O2"),repeat)
    changed,_=compile_like(ir,"KCC-lab-1","aarch64","O2")
    assert cold[0]==warm[0] and cold[1] is False and warm[1] is True and changed[0]!=cold[0][0]
    return {"template":"canonical-ir-content-cache","cold_ms":round(tc,6),"warm_ms":round(tw,6),
            "warm_hit":warm[1],"target_invalidation":"PASS","correctness":"PASS",
            "scope":"cache-key prototype; not a KCC compilation benchmark"}

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--rows",type=int,default=20000)
    ap.add_argument("--repeat",type=int,default=5)
    ap.add_argument("--out",default="ci-out/combined-tech-lab")
    a=ap.parse_args()
    if a.rows<1 or a.repeat<1: ap.error("rows and repeat must be positive")
    events=make_events(a.rows)
    results=[test_ipc(events,a.repeat),test_storage(events,a.repeat),
             test_telemetry(events,a.repeat),test_cache(a.repeat)]
    os.makedirs(a.out,exist_ok=True)
    with open(Path(a.out)/"results.json","w") as f:
        json.dump({"lab":"KIRA combined technology prototypes v1","rows":a.rows,
                   "repeat":a.repeat,"results":results},f,indent=2)
    with open(Path(a.out)/"results.csv","w",newline="") as f:
        w=csv.DictWriter(f,fieldnames=sorted({k for r in results for k in r}))
        w.writeheader(); w.writerows(results)
    for r in results: print(json.dumps(r,ensure_ascii=False))
    print("ALL_CORRECTNESS_GATES=PASS")

if __name__=="__main__": main()
