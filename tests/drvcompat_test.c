#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../userland/drvcompat.h"

struct test_ctx {
 unsigned char archive[8],source_data[8],native_data[8],binary[8],package[8];
 size_t archive_size;
 int fail_stage,fetches,verified,unpacked,inspected,translated,built,signed_n,published;
 int archive_releases,buffer_releases,progress_errors;
 struct kira_drvcompat_manifest source,native;
};
static int hash_data(void *v,const void *data,size_t n,uint8_t out[32])
{
 const uint8_t *p=data;uint32_t h=2166136261u;size_t i;(void)v;
 for(i=0;i<n;i++)h=(h^p[i])*16777619u;
 for(i=0;i<32;i++){h^=h>>13;h*=0x5bd1e995u;out[i]=(uint8_t)(h>>(i%4)*8);}
 return 0;
}
static void digest_file(struct test_ctx *f,struct kira_drvcompat_file *file)
{hash_data(f,file->data,file->size,file->digest);}
static void make_manifest(struct test_ctx *f,struct kira_drvcompat_manifest *m,
                          const char *name,const char *os,const char *arch,
                          const char *path,const uint8_t *data,size_t size)
{
 memset(m,0,sizeof(*m));snprintf(m->name,sizeof(m->name),"%s",name);
 snprintf(m->source_os,sizeof(m->source_os),"%s",os);
 snprintf(m->target_arch,sizeof(m->target_arch),"%s",arch);
 m->file_count=1;snprintf(m->files[0].path,sizeof(m->files[0].path),"%s",path);
 m->files[0].data=data;m->files[0].size=size;digest_file(f,&m->files[0]);
}
static int fetch(void *v,const char *uri,void **a,size_t *n)
{struct test_ctx *f=v;(void)uri;f->fetches++;*a=f->archive;*n=f->archive_size;return f->fail_stage==1?-1:0;}
static void release_archive(void *v,void *a,size_t n)
{struct test_ctx *f=v;(void)a;(void)n;f->archive_releases++;}
static int verify(void *v,const void *a,size_t n)
{struct test_ctx *f=v;(void)a;(void)n;f->verified++;return f->fail_stage==2?-1:0;}
static int unpack(void *v,const void *a,size_t n,struct kira_drvcompat_manifest *m)
{struct test_ctx *f=v;(void)a;(void)n;f->unpacked++;*m=f->source;return f->fail_stage==3?-1:0;}
static int inspect(void *v,const struct kira_drvcompat_manifest *m)
{struct test_ctx *f=v;(void)m;f->inspected++;return f->fail_stage==4?-1:0;}
static int translate(void *v,const struct kira_drvcompat_manifest *s,struct kira_drvcompat_manifest *m)
{struct test_ctx *f=v;(void)s;f->translated++;*m=f->native;return f->fail_stage==5?-1:0;}
static int build(void *v,const struct kira_drvcompat_manifest *m,void **b,size_t *n)
{struct test_ctx *f=v;(void)m;f->built++;*b=f->binary;*n=4;return f->fail_stage==6?-1:0;}
static int sign_pkg(void *v,const char *name,const void *b,size_t n,void **p,size_t *pn)
{struct test_ctx *f=v;(void)name;(void)b;(void)n;f->signed_n++;*p=f->package;*pn=4;return f->fail_stage==7?-1:0;}
static int publish(void *v,const char *name,const void *p,size_t n)
{struct test_ctx *f=v;(void)name;(void)p;(void)n;f->published++;return f->fail_stage==8?-1:0;}
static void release_buffer(void *v,void *p,size_t n)
{struct test_ctx *f=v;(void)p;(void)n;f->buffer_releases++;}
static void progress(void *v,enum kira_drvcompat_stage s,int rc)
{struct test_ctx *f=v;(void)s;if(rc)f->progress_errors++;}
static void setup(struct test_ctx *f,struct kira_drvcompat_ops *o)
{
 memset(f,0,sizeof(*f));f->archive_size=sizeof(f->archive);
 make_manifest(f,&f->source,"rtl8139","linux","x86_64","src/rtl8139.c",f->source_data,4);
 make_manifest(f,&f->native,"rtl8139","kira","x86_64","src/rtl8139.c",f->native_data,4);
 memset(o,0,sizeof(*o));o->context=f;o->fetch_archive=fetch;o->release_archive=release_archive;
 o->verify_publisher=verify;o->unpack_safe=unpack;o->hash=hash_data;o->inspect_source=inspect;
 o->translate_driver=translate;o->build_native=build;o->sign_native=sign_pkg;
 o->publish_package=publish;o->release_buffer=release_buffer;o->progress=progress;
}
static void success_path(void)
{
 struct test_ctx f;struct kira_drvcompat_ops o;setup(&f,&o);
 assert(kira_drvcompat_convert(&o,"https://drivers.example/rtl8139.kira-src")==0);
 assert(f.fetches==1&&f.verified==1&&f.unpacked==1&&f.inspected==1);
 assert(f.translated==1&&f.built==1&&f.signed_n==1&&f.published==1);
 assert(f.archive_releases==1&&f.buffer_releases==2&&f.progress_errors==0);
}
static void rejected_inputs(void)
{
 struct test_ctx f;struct kira_drvcompat_ops o;setup(&f,&o);
 assert(kira_drvcompat_convert(&o,"http://drivers.example/a")==-1);
 assert(f.fetches==0);
 setup(&f,&o);snprintf(f.source.files[0].path,sizeof(f.source.files[0].path),"../escape.c");
 assert(kira_drvcompat_convert(&o,"https://drivers.example/a")==-1);
 assert(f.inspected==0&&f.built==0&&f.archive_releases==1);
 setup(&f,&o);snprintf(f.native.name,sizeof(f.native.name),"../escape");
 assert(kira_drvcompat_convert(&o,"https://drivers.example/a")==-1);
 assert(f.built==0&&f.published==0);
 setup(&f,&o);f.source.file_count=2;f.source.files[1]=f.source.files[0];
 assert(kira_drvcompat_convert(&o,"https://drivers.example/a")==-1);
 assert(f.inspected==0);
 setup(&f,&o);f.source.file_count=3;
 for(unsigned i=0;i<3;i++){f.source.files[i].size=KIRA_DRVCOMPAT_MAX_FILE_SIZE;f.source.files[i].data=f.source_data;}
 assert(kira_drvcompat_convert(&o,"https://drivers.example/a")==-1);
 assert(f.inspected==0);
 setup(&f,&o);f.archive_size=KIRA_DRVCOMPAT_MAX_ARCHIVE_SIZE+1u;
 assert(kira_drvcompat_convert(&o,"https://drivers.example/a")==-1);
 assert(f.verified==0&&f.archive_releases==1);
}
static void fail_closed(void)
{
 for(int stage=1;stage<=8;stage++){
  struct test_ctx f;struct kira_drvcompat_ops o;setup(&f,&o);f.fail_stage=stage;
  assert(kira_drvcompat_convert(&o,"https://drivers.example/a")!=0);
  assert(f.published==(stage==8?1:0));
  assert(f.archive_releases==1);
  assert(f.progress_errors==1);
 }
}
int main(void)
{success_path();rejected_inputs();fail_closed();puts("drvcompat: HTTPS gate, bounded manifests, path/name validation, ordered fail-closed pipeline passed");return 0;}
