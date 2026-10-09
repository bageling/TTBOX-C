// fp16_probe.cpp v2 — 稳健解析 dims, 试多种输入约定, 打印 person(cls0) logit max
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <cstdint>
#include "rknn_api.h"

static uint16_t f32_to_half(float f){uint32_t b;memcpy(&b,&f,4);uint32_t s=(b>>16)&0x8000u;int e=((b>>23)&0xff)-127+15;uint32_t m=b&0x7fffffu;if(e>=31)return(uint16_t)(s|0x7c00u);if(e<=0)return(uint16_t)(s);m>>=13;return(uint16_t)(s|(((uint32_t)e)<<10)|m);}
static float half_to_f32(uint16_t h){uint32_t s=((uint32_t)h&0x8000u)<<16;uint32_t e=(h>>10)&0x1Fu;uint32_t m=h&0x3FFu;uint32_t b;if(e==0){if(m==0)b=s;else{uint32_t ee=127-15+1,mm=m;while((mm&0x400u)==0){mm<<=1;ee--;}mm&=0x3FFu;b=s|((ee+15)<<23)|(mm<<13);}}else if(e==0x1F)b=s|0x7F800000u|(m<<13);else b=s|((e-15+127)<<23)|(m<<13);float f;memcpy(&f,&b,4);return f;}

int main(int argc,char**argv){
    if(argc<3){fprintf(stderr,"usage: %s <model> <raw>\n",argv[0]);return 1;}
    FILE*f=fopen(argv[2],"rb");if(!f){fprintf(stderr,"open fail\n");return 1;}
    // 尝试 500x500 或 640x640 BGR raw
    std::vector<uint8_t>src; size_t srcn=0;
    for( auto dim : {640u,500u,416u} ){ size_t sz=dim*dim*3; std::vector<uint8_t>tmp(sz); rewind(f); if(fread(tmp.data(),1,sz,f)==sz){src=tmp;srcn=dim;break;} }
    if(srcn==0){fprintf(stderr,"raw 尺寸不匹配\n");return 1;}
    printf("src=%ux%u\n",(unsigned)srcn,(unsigned)srcn);
    fclose(f);
    const uint32_t SRC=srcn;

    rknn_context ctx=0;int ret=rknn_init(&ctx,(void*)argv[1],0,0,NULL);if(ret){fprintf(stderr,"init %d\n",ret);return 1;}
    rknn_input_output_num io;rknn_query(ctx,RKNN_QUERY_IN_OUT_NUM,&io,sizeof(io));
    rknn_tensor_attr in;memset(&in,0,sizeof(in));in.index=0;rknn_query(ctx,RKNN_QUERY_INPUT_ATTR,&in,sizeof(in));
    /* NHWC [1,H,W,C] : dims[1]=H dims[2]=W dims[3]=C ; NCHW [1,C,H,W]: dims[1]=C */
    int Hc = (in.fmt==RKNN_TENSOR_NHWC)?in.dims[1]:in.dims[2];
    int Wc = (in.fmt==RKNN_TENSOR_NHWC)?in.dims[2]:in.dims[3];
    printf("input fmt=%d(type解读) dims=[%d,%d,%d,%d] H=%d W=%d type=%d\n",in.fmt,in.dims[0],in.dims[1],in.dims[2],in.dims[3],Hc,Wc,in.type);

    // center crop SRC->Wc square nearest
    std::vector<uint8_t> img(Wc*Wc*3);
    for(int y=0;y<Wc;++y)for(int x=0;x<Wc;++x){int sy=std::min<int>(SRC-1,(y*SRC)/Wc),sx=std::min<int>(SRC-1,(x*SRC)/Wc);memcpy(&img[(y*Wc+x)*3],&src[(sy*SRC+sx)*3],3);}

    rknn_tensor_attr out;memset(&out,0,sizeof(out));out.index=0;rknn_query(ctx,RKNN_QUERY_OUTPUT_ATTR,&out,sizeof(out));
    int C=out.dims[1],M=out.n_elems/C;

    auto report=[&](const char*n,void*buf){
        std::vector<uint8_t> ob(out.size);rknn_output o;memset(&o,0,sizeof(o));o.index=0;o.want_float=0;o.buf=ob.data();o.size=out.size;
        rknn_inputs_set(ctx,1,(rknn_input*)buf); // placeholder
    };

    // 构造输入建议 frame-builder: 对每种 (type,fmt,color,scale) 打印 person logit max
    auto probe=[&](const char*name,rknn_tensor_type type,rknn_tensor_format fmt,int color_rgb,float scale){
        size_t ne=Wc*Wc*3;
        std::vector<uint8_t> intbuf; std::vector<float> f32buf; std::vector<uint16_t> hbuf;
        void* mem=NULL; size_t sz=0;
        // 构造 NHWC 平面
        std::vector<uint8_t> nhwc(ne); { // 像素序按 format 决定 通道序(color_rgb)
            for(int y=0;y<Wc;++y)for(int x=0;x<Wc;++x){
                int i=(y*Wc+x)*3;
                uint8_t b=img[i+0],g=img[i+1],r=img[i+2];
                nhwc[i+0]=color_rgb?r:b; nhwc[i+1]=g; nhwc[i+2]=color_rgb?b:r;
            }
        }
        if(type==RKNN_TENSOR_FLOAT32){ f32buf.resize(ne); for(int i=0;i<(int)ne;++i)f32buf[i]= (scale==1.0f)?(float)nhwc[i]/255.0f:(float)nhwc[i]; mem=f32buf.data();sz=ne*4; }
        else if(type==RKNN_TENSOR_FLOAT16){ hbuf.resize(ne); for(int i=0;i<(int)ne;++i)hbuf[i]=f32_to_half((scale==1.0f)?(float)nhwc[i]/255.0f:(float)nhwc[i]); mem=hbuf.data();sz=ne*2; }
        else { intbuf=nhwc; mem=intbuf.data();sz=ne; }
        rknn_input in;memset(&in,0,sizeof(in));in.index=0;in.type=type;in.fmt=fmt;in.size=sz;in.buf=mem;
        int r1=rknn_inputs_set(ctx,1,&in); if(r1){printf("[%s] inputs_set fail %d\n",name,r1);return;}
        int r2=rknn_run(ctx,NULL); if(r2){printf("[%s] run fail %d\n",name,r2);return;}
        std::vector<uint8_t>ob(out.size);rknn_output o;memset(&o,0,sizeof(o));o.index=0;o.want_float=0;o.buf=ob.data();o.size=out.size;o.is_prealloc=1;
        int r3=rknn_outputs_get(ctx,1,&o,NULL); if(r3){printf("[%s] get fail %d\n",name,r3);return;}
        float maxc=-1e30f;int mch=-1;float person=-1e30f;float cxSum=0;int cxN=0;
        for(int ch=4;ch<C;++ch)for(int a=0;a<M;++a){float v=half_to_f32(((uint16_t*)ob.data())[(size_t)ch*M+a]); if(v>maxc){maxc=v;mch=ch;}}
        // 统计各 cls 通道最大值（仅 cls 通道 ch>=4, 排除坐标 ch0-3）
        float clsmax_all=-1e30f; int cmax_ch=-2;
        for(int ch=4;ch<C;++ch){ float mx=-1e30f; for(int a=0;a<M;++a){float v=half_to_f32(((uint16_t*)ob.data())[(size_t)ch*M+a]); if(v>mx)mx=v;} if(mx>clsmax_all){clsmax_all=mx;cmax_ch=ch-4;} }
        // 统计各 cls 通道 top5 anchor 的 logit 均值（排除坐标）
        float topk=0; int topk_n=0;
        for(int ch=4;ch<C;++ch){ float best[6]={-1e30f,-1e30f,-1e30f,-1e30f,-1e30f,-1e30f}; for(int a=0;a<M;++a){float v=half_to_f32(((uint16_t*)ob.data())[(size_t)ch*M+a]); for(int k=0;k<6;++k)if(v>best[k]){for(int kk=5;kk>k;--kk)best[kk]=best[kk-1];best[k]=v;break;}} for(int k=0;k<6;++k){topk+=best[k];topk_n++;} }
        for(int a=0;a<M;++a){float v=half_to_f32(((uint16_t*)ob.data())[(size_t)4*M+a]); if(v>person)person=v; if(v<10&&v>-10){cxSum+=v;cxN++;}}
        printf("[%-24s] clsmax=%6.2f@cls%-3d personmax=%5.2f cls0avg=%6.3f top6percls_avg=%6.3f\n",name,clsmax_all,cmax_ch,person,cxN?cxSum/cxN:0,topk_n?topk/topk_n:0);
        rknn_outputs_release(ctx,1,&o);
    };

    // 打印输出属性
    printf("OUT: type=%d C=%d M=%d size=%d\n", out.type,C,M,out.size);

    probe("FP16 NHWC BGR /255",RKNN_TENSOR_FLOAT16,RKNN_TENSOR_NHWC,0,1.0f);
    probe("FP16 NHWC RGB /255",RKNN_TENSOR_FLOAT16,RKNN_TENSOR_NHWC,1,1.0f);
    probe("FP32 NHWC BGR /255",RKNN_TENSOR_FLOAT32,RKNN_TENSOR_NHWC,0,1.0f);
    probe("FP32 NHWC RGB /255",RKNN_TENSOR_FLOAT32,RKNN_TENSOR_NHWC,1,1.0f);
    probe("UINT8 NHWC BGR raw",RKNN_TENSOR_UINT8,RKNN_TENSOR_NHWC,0,0.0f);
    probe("UINT8 NHWC RGB raw",RKNN_TENSOR_UINT8,RKNN_TENSOR_NHWC,1,0.0f);
    probe("FP16 NHWC BGR raw",RKNN_TENSOR_FLOAT16,RKNN_TENSOR_NHWC,0,0.0f);
    rknn_destroy(ctx);
    return 0;
}