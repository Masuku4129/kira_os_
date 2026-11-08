/*
 * KIRA driver compatibility conversion pipeline.
 *
 * This is a fail-closed orchestration layer. It never executes foreign driver
 * binaries, never accepts unsafe extracted paths, and only publishes a native
 * package after publisher authentication, source inspection, translation,
 * native build and local signing all succeed.
 */
#include "drvcompat.h"

static int str_ok(const char *s,size_t cap)
{
 size_t i;if(!s||!cap)return 0;
 for(i=0;i<cap;i++)if(!s[i])return i!=0;
 return 0;
}
static int token_safe(const char *s,size_t cap)
{
 size_t i;
 if(!str_ok(s,cap)||s[0]=='.')return 0;
 for(i=0;s[i];i++){
  char c=s[i];
  if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||
       (c>='0'&&c<='9')||c=='_'||c=='-'||c=='.'))return 0;
 }
 return 1;
}
static int uri_https_safe(const char *s)
{
 size_t i,host=0;
 if(!str_ok(s,1024)||s[0]!='h'||s[1]!='t'||s[2]!='t'||
    s[3]!='p'||s[4]!='s'||s[5]!=':'||s[6]!='/'||s[7]!='/')return 0;
 for(i=8;s[i];i++){
  unsigned char c=(unsigned char)s[i];
  if(c<=0x20||c==0x7f||c=='\\'||c=='@')return 0;
  if(s[i]=='/'||s[i]=='?'||s[i]=='#')break;
  host++;
 }
 return host!=0;
}
static int path_safe(const char *p)
{
 size_t i=0,component=0;
 if(!str_ok(p,192)||p[0]=='/'||p[0]=='\\')return 0;
 while(p[i]){
  if(p[i]=='\\'||p[i]==':')return 0;
  if(p[i]=='/'){
   if(component==0)return 0;
   if((component==1&&p[i-1]=='.')||
      (component==2&&p[i-1]=='.'&&p[i-2]=='.'))return 0;
   component=0;
  }else component++;
  i++;
 }
 if(!component)return 0;
 if((component==1&&p[i-1]=='.')||
    (component==2&&p[i-1]=='.'&&p[i-2]=='.'))return 0;
 return 1;
}
static void report(const struct kira_drvcompat_ops *o,
                   enum kira_drvcompat_stage s,int rc)
{if(o->progress)o->progress(o->context,s,rc);}
static void clear_manifest(struct kira_drvcompat_manifest *m)
{
 unsigned char *p=(unsigned char *)m;size_t i;
 for(i=0;i<sizeof(*m);i++)p[i]=0;
}
static int validate_manifest(const struct kira_drvcompat_manifest *m)
{
 size_t i,j,total=0;
 if(!m||!token_safe(m->name,sizeof(m->name))||
    !token_safe(m->source_os,sizeof(m->source_os))||
    !token_safe(m->target_arch,sizeof(m->target_arch))||
    !m->file_count||m->file_count>KIRA_DRVCOMPAT_MAX_FILES)return -1;
 for(i=0;i<m->file_count;i++){
  const struct kira_drvcompat_file *f=&m->files[i];
  if(!path_safe(f->path)||!f->data||!f->size||
     f->size>KIRA_DRVCOMPAT_MAX_FILE_SIZE||
     f->size>KIRA_DRVCOMPAT_MAX_TOTAL_SIZE-total)return -2;
  total+=f->size;
  for(j=0;j<i;j++)if(!__builtin_strcmp(f->path,m->files[j].path))return -3;
 }
 return 0;
}
static int verify_files(const struct kira_drvcompat_ops *o,
                        const struct kira_drvcompat_manifest *m)
{
 size_t i;uint8_t digest[32];
 for(i=0;i<m->file_count;i++){
  if(o->hash(o->context,m->files[i].data,m->files[i].size,digest)!=0)return -1;
  {
   unsigned j;uint8_t diff=0;
   for(j=0;j<32;j++)diff|=(uint8_t)(digest[j]^m->files[i].digest[j]);
   if(diff)return -2;
  }
 }
 return 0;
}
int kira_drvcompat_convert(const struct kira_drvcompat_ops *o,const char *uri)
{
 void *archive=0,*binary=0,*package=0;
 size_t archive_size=0,binary_size=0,package_size=0;
 struct kira_drvcompat_manifest source,native;
 enum kira_drvcompat_stage failed_stage=KIRA_DRV_FETCHED;
 int rc=-1;
 if(!o||!uri_https_safe(uri)||!o->fetch_archive||!o->release_archive||
    !o->verify_publisher||!o->unpack_safe||!o->hash||
    !o->inspect_source||!o->translate_driver||!o->build_native||
    !o->sign_native||!o->publish_package||!o->release_buffer)return -1;
 clear_manifest(&source);clear_manifest(&native);
 if(o->fetch_archive(o->context,uri,&archive,&archive_size)!=0||
    !archive||!archive_size||archive_size>KIRA_DRVCOMPAT_MAX_ARCHIVE_SIZE)goto done;
 report(o,KIRA_DRV_FETCHED,0);
 failed_stage=KIRA_DRV_AUTHENTICATED;
 if(o->verify_publisher(o->context,archive,archive_size)!=0)goto done;
 report(o,KIRA_DRV_AUTHENTICATED,0);
 failed_stage=KIRA_DRV_INSPECTED;
 if(o->unpack_safe(o->context,archive,archive_size,&source)!=0||
    validate_manifest(&source)!=0||verify_files(o,&source)!=0)goto done;
 if(o->inspect_source(o->context,&source)!=0)goto done;
 report(o,KIRA_DRV_INSPECTED,0);
 failed_stage=KIRA_DRV_TRANSLATED;
 if(o->translate_driver(o->context,&source,&native)!=0||
    validate_manifest(&native)!=0||verify_files(o,&native)!=0)goto done;
 report(o,KIRA_DRV_TRANSLATED,0);
 failed_stage=KIRA_DRV_BUILT;
 if(o->build_native(o->context,&native,&binary,&binary_size)!=0||
    !binary||!binary_size||binary_size>KIRA_DRVCOMPAT_MAX_OUTPUT_SIZE)goto done;
 report(o,KIRA_DRV_BUILT,0);
 failed_stage=KIRA_DRV_SIGNED;
 if(o->sign_native(o->context,native.name,binary,binary_size,
                   &package,&package_size)!=0||!package||!package_size||
    package_size>KIRA_DRVCOMPAT_MAX_OUTPUT_SIZE)goto done;
 report(o,KIRA_DRV_SIGNED,0);
 failed_stage=KIRA_DRV_PUBLISHED;
 if(o->publish_package(o->context,native.name,package,package_size)!=0)goto done;
 report(o,KIRA_DRV_PUBLISHED,0);rc=0;
done:
 if(package)o->release_buffer(o->context,package,package_size);
 if(binary)o->release_buffer(o->context,binary,binary_size);
 if(archive)o->release_archive(o->context,archive,archive_size);
 if(rc)report(o,failed_stage,rc);
 return rc;
}
