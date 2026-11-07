/*
 * KIRA driver compatibility conversion pipeline.
 *
 * This is a fail-closed orchestration layer. It never executes foreign driver
 * binaries, never accepts path traversal in extracted file names, and only
 * publishes a native package after publisher authentication, source inspection,
 * target translation, native build and local signing all succeed.
 */
#include "drvcompat.h"

static int str_ok(const char *s,size_t cap)
{
 size_t i;if(!s||!cap)return 0;
 for(i=0;i<cap;i++)if(!s[i])return i!=0;
 return 0;
}
static int path_safe(const char *p)
{
 size_t i=0,component=0;
 if(!str_ok(p,192)||p[0]=='/'||p[0]=='\\')return 0;
 while(p[i]){
  if(p[i]=='\\')return 0;
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
static int validate_manifest(const struct kira_drvcompat_manifest *m)
{
 size_t i;
 if(!m||!str_ok(m->name,sizeof(m->name))||
    !str_ok(m->source_os,sizeof(m->source_os))||
    !str_ok(m->target_arch,sizeof(m->target_arch))||
    !m->file_count||m->file_count>KIRA_DRVCOMPAT_MAX_FILES)return -1;
 for(i=0;i<m->file_count;i++){
  if(!path_safe(m->files[i].path)||!m->files[i].data||
     !m->files[i].size||m->files[i].size>KIRA_DRVCOMPAT_MAX_FILE_SIZE)return -2;
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
 int rc=-1;
 if(!o||!str_ok(uri,1024)||!o->fetch_archive||!o->release_archive||
    !o->verify_publisher||!o->unpack_safe||!o->hash||
    !o->inspect_source||!o->translate_driver||!o->build_native||
    !o->sign_native||!o->publish_package||!o->release_buffer)return -1;
 if(o->fetch_archive(o->context,uri,&archive,&archive_size)!=0||
    !archive||!archive_size)goto done;
 report(o,KIRA_DRV_FETCHED,0);
 if(o->verify_publisher(o->context,archive,archive_size)!=0)goto done;
 report(o,KIRA_DRV_AUTHENTICATED,0);
 if(o->unpack_safe(o->context,archive,archive_size,&source)!=0||
    validate_manifest(&source)!=0||verify_files(o,&source)!=0)goto done;
 if(o->inspect_source(o->context,&source)!=0)goto done;
 report(o,KIRA_DRV_INSPECTED,0);
 if(o->translate_driver(o->context,&source,&native)!=0||
    validate_manifest(&native)!=0||verify_files(o,&native)!=0)goto done;
 report(o,KIRA_DRV_TRANSLATED,0);
 if(o->build_native(o->context,&native,&binary,&binary_size)!=0||
    !binary||!binary_size)goto done;
 report(o,KIRA_DRV_BUILT,0);
 if(o->sign_native(o->context,native.name,binary,binary_size,
                   &package,&package_size)!=0||!package||!package_size)goto done;
 report(o,KIRA_DRV_SIGNED,0);
 if(o->publish_package(o->context,native.name,package,package_size)!=0)goto done;
 report(o,KIRA_DRV_PUBLISHED,0);rc=0;
done:
 if(package)o->release_buffer(o->context,package,package_size);
 if(binary)o->release_buffer(o->context,binary,binary_size);
 if(archive)o->release_archive(o->context,archive,archive_size);
 if(rc)report(o,KIRA_DRV_PUBLISHED,rc);
 return rc;
}
