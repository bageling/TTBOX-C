// yolo_probe.c — 决定性诊断：yolo261n 输入变体实验（纯 rknn C API，不碰 TTBOX 核心）
// 读 BGR24 raw → 构造多种输入喂 yolo261n，打印输出通道统计，判定哪种输入有效。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "rknn_api.h"

// FP16 -> float
static float f16(uint16_t h){
    uint32_t s=((uint32_t)h&0x8000u)<<16;
    uint32_t e=(h>>10)&0x1Fu, m=h&0x3FFu, bits;
    if(e==0){ if(m==0) bits=s; else {uint32_t ee=127-15+1,mm=m; while((mm&0x400u)==0){mm<<=1;ee--;} mm&=0x3FFu; bits=s|((ee+15)<<23)|(mm<<13);} }
    else if(e==0x1Fu) bits=s|0x7F800000u|(m<<13);
    else bits=s|((e-15+127)<<23)|(m<<13);
    float f; memcpy(&f,&bits,4); return f;
}
static uint16_t f2h(float f){
    uint32_t b; memcpy(&b,&f,4);
    uint32_t s=(b>>16)&0x8000u; int e=((b>>23)&0xff)-127+15; uint32_t m=b&0x7fffff;
    if(e>=31) return (uint16_t)(s|0x7c00u);
    if(e<=0) return (uint16_t)s;
    m>>=13;
    return (uint16_t)(s|((uint32_t)e<<10)|m);
}

int main(int argc, char** argv){
    if(argc<5){ fprintf(stderr,"usage: %s <model> <raw_bgr> <W> <H> [mode] [scale]\n  mode: 0=BGR/255half 1=RGB/255half 2=BGR rawuint8 3=RGB rawuint8\n", argv[0]); return 2; }
    const char* model=argv[1]; const char* rawp=argv[2];
    int W=atoi(argv[3]), H=atoi(argv[4]);
    int mode = argc>5?atoi(argv[5]):0;
    float scale = argc>6?atof(argv[6]):(1.0f/255.0f);

    FILE* f=fopen(rawp,"rb"); if(!f){fprintf(stderr,"raw open fail\n");return 3;}
    size_t n=(size_t)W*H*3; uint8_t* px=malloc(n); if(n!=fread(px,1,n,f)){fprintf(stderr,"read %zu\n",n);} fclose(f);

    rknn_context ctx=0;
    int rc=rknn_init(&ctx, (char*)model, 0, 0, NULL);
    if(rc!=RKNN_SUCC){fprintf(stderr,"rknn_init fail %d\n",rc);return 4;}

    rknn_input_output_num ionum; rknn_query(ctx,RKNN_QUERY_IN_OUT_NUM,&ionum,sizeof(ionum));
    rknn_tensor_attr ina; memset(&ina,0,sizeof(ina)); ina.index=0;
    rknn_query(ctx,RKNN_QUERY_INPUT_ATTR,&ina,sizeof(ina));
    int iw=ina.dims[2], ih=ina.dims[1]; // NHWC
    printf("model in=%dx%d type=%d fmt=%d size=%d | outs=%d\n", iw,ih,ina.type,ina.fmt,ina.size, ionum.n_output);
    for(uint32_t i=0;i<ionum.n_output;i++){
        rknn_tensor_attr oa; memset(&oa,0,sizeof(oa)); oa.index=i;
        rknn_query(ctx,RKNN_QUERY_OUTPUT_ATTR,&oa,sizeof(oa));
        printf("out[%u] type=%d fmt=%d size=%d n_elems=%u dims=[", i,oa.type,oa.fmt,oa.size,oa.n_elems);
        for(int d=0;d<oa.n_dims;d++) printf("%u%s", oa.dims[d],d+1<oa.n_dims?",":"");
        printf("] zp=%d scale=%f\n", oa.zp, oa.scale);
    }

    // ---- 构造输入 ----
    // 采样：视频帧大小可能不等于模型尺寸（此工具假设 raw 全图，中心ROI缩到模型）
    // 简化：直接对全图做中心 crop + 最近邻缩放到模型尺寸
    size_t inN=(size_t)iw*ih*3;
    uint8_t* cpu=malloc(inN);
    // 中心crop到cropw
    int cw=W<H?W:H; int cw2=500; // 用500x500 ROI
    int cropx=W/2-cw2/2, cropy=H/2-cw2/2;
    for(int y=0;y<ih;y++) for(int x=0;x<iw;x++){
        int sy=cropy+(int)((float)y/(float)ih*cw2);
        int sx=cropx+(int)((float)x/(float)iw*cw2);
        const uint8_t* src=px+((size_t)sy*W+sx)*3;
        uint8_t* dst=cpu+((size_t)y*iw+x)*3;
        // BGR source; mode1/3 => swap to RGB
        if(mode==1||mode==3){ dst[0]=src[2]; dst[1]=src[1]; dst[2]=src[0]; }
        else { dst[0]=src[0]; dst[1]=src[1]; dst[2]=src[2]; }
    }
    // 对 FP16 模型：转 half 并乘 scale；对 INT8/UINT8：保留原始量或乘 255
    rknn_input in; memset(&in,0,sizeof(in));
    in.index=0; in.fmt=RKNN_TENSOR_NHWC;
    uint16_t* halfbuf=NULL;
    uint8_t* u8buf=NULL;
    if(mode==2||mode==3){
        // UINT8 raw (0-255)
        in.type=RKNN_TENSOR_UINT8; in.size=(uint32_t)inN; in.buf=cpu;
    } else {
        // FLOAT16, scale归一化
        halfbuf=malloc(inN*2); uint16_t* hp=halfbuf;
        for(size_t i=0;i<inN;i++) hp[i]=f2h((float)cpu[i]*scale);
        in.type=RKNN_TENSOR_FLOAT16; in.size=(uint32_t)(inN*2); in.buf=halfbuf;
    }
    in.pass_through=0;
    rc=rknn_inputs_set(ctx,1,&in); if(rc){fprintf(stderr,"inputs_set fail %d mode=%d\n",rc,mode);return 5;}
    rc=rknn_run(ctx,NULL); if(rc){fprintf(stderr,"run fail %d\n",rc);return 6;}

    // dump first output
    rknn_tensor_attr oa0; memset(&oa0,0,sizeof(oa0)); oa0.index=0;
    rknn_query(ctx,RKNN_QUERY_OUTPUT_ATTR,&oa0,sizeof(oa0));
    void* obuf=malloc(oa0.size);
    rknn_output out; memset(&out,0,sizeof(out)); out.index=0; out.want_float=0; out.is_prealloc=1; out.buf=obuf; out.size=oa0.size;
    rc=rknn_outputs_get(ctx,1,&out,NULL); if(rc){fprintf(stderr,"out get fail %d\n",rc);return 7;}
    const uint8_t* b=(const uint8_t*)obuf;
    uint32_t M=1; for(int d=2;d<oa0.n_dims;d++) M*=oa0.dims[d];
    uint32_t C=oa0.dims[1];
    int ncls=(int)C-4;
    // 统计 per-channel avg (C*M layout)
    printf("MODE=%d scale=%f : C=%u M=%u n_elems=%u\n", mode, scale, C, M, oa0.n_elems);
    // 找最大 cls 分 + 位置
    float globalmax=-1e30f; int gmax_anchor=-1,gmax_ch=-1;
    for(uint32_t c=4;c<C;c++) for(uint32_t a=0;a<M;a++){
        float v = (oa0.type==RKNN_TENSOR_FLOAT16)? f16(((uint16_t*)b)[(size_t)c*M+a]) : (float)((int8_t*)b)[(size_t)c*M+a];
        if(oa0.type==RKNN_TENSOR_INT8) v=(v-(float)oa0.zp)*oa0.scale;
        if(v>globalmax){globalmax=v;gmax_anchor=a;gmax_ch=c;}
    }
    float maxcls=1.0f/(1.0f+expf(-globalmax));
    printf("mode=%d max_cls_raw=%f (sigmoid=%f) anchor=%d ch=%d (class %d)\n", mode, globalmax, maxcls, gmax_anchor, gmax_ch, gmax_ch-4);
    // per-channel avg first 6 and cls avg cluster
    for(uint32_t ch=0;ch<C && ch<12;ch++){ double s2=0; if(oa0.type==RKNN_TENSOR_FLOAT16){for(uint32_t a=0;a<M;a++) s2+=f16(((uint16_t*)b)[ch*M+a]);} else {for(uint32_t a=0;a<M;a++){float v=(float)((int8_t*)b)[ch*M+a]; s2+=(v-(float)oa0.zp)*oa0.scale;}} printf("  ch%u avg=%.3f\n",ch,s2/M); }
    // 统计 cls 通道最大值分布 ps: 打印 top anchors 的类分数
    printf("-- top-5 anchors by cls --\n");
    float* clscpy=malloc(M*sizeof(float)); for(uint32_t a=0;a<M;a++){float best=-1;for(uint32_t c=4;c<C;c++){float v=(oa0.type==RKNN_TENSOR_FLOAT16)?f16(((uint16_t*)b)[c*M+a]):(float)((int8_t*)b)[c*M+a]; if(oa0.type==RKNN_TENSOR_INT8)v=(v-(float)oa0.zp)*oa0.scale; if(v>best)best=v;} clscpy[a]=best;}
    for(int k=0;k<5;k++){float best=-1e30f;int ba=0;for(uint32_t a=0;a<M;a++)if(clscpy[a]>best){best=clscpy[a];ba=a;}printf("  rank%d anchor=%d raw_cls=%f sig=%f\n",k,ba,best,1.0f/(1.0f+expf(-best)));clscpy[ba]=-1e30f;}

    rknn_outputs_release(ctx,1,&out);
    rknn_destroy(ctx);
    free(px);free(cpu);free(obuf);free(halfbuf);free(u8buf);free(clscpy);
    return 0;
}