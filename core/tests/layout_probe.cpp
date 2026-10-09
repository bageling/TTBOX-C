// layout_probe.cpp - 验证 yolo261n 输出布局与输入约定
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <cstdint>
#include "rknn_api.h"
static uint16_t f2h(float f){uint32_t b;memcpy(&b,&f,4);uint32_t s=(b>>16)&0x8000u;int e=((b>>23)&0xff)-127+15;uint32_t m=b&0x7fffffu;if(e>=31)return(uint16_t)(s|0x7c00u);if(e<=0)return(uint16_t)s;m>>=13;return(uint16_t)(s|(((uint32_t)e)<<10)|m);}
static float h2f(uint16_t h){uint32_t s=((uint32_t)h&0x8000u)<<16;uint32_t e=(h>>10)&0x1Fu,n=h&0x3FFu,b;if(e==0){if(n==0)b=s;else{uint32_t ee=127-15+1,nn=n;while((nn&0x400u)==0){nn<<=1;ee--;}nn&=0x3FFu;b=s|((ee+15)<<23)|(nn<<13);}}else if(e==0x1F)b=s|0x7F800000u|(n<<13);else b=s|((e-15+127)<<23)|(n<<13);float f;memcpy(&f,&b,4);return f;}
int main(int ac,char**av){
 if(ac<3)return 1; FILE*f=fopen(av[2],"rb");if(!f)return 1;
 std::vector<uint8_t>src; int srcn=0;
 for(auto d:{500u,640u}){size_t z=d*d*3;std::vector<uint8_t>t(z);rewind(f);if(fread(t.data(),1,z,f)==z){src=t;srcn=d;break;}}
 if(srcn==0)return 1; fclose(f); const uint32_t SRC=srcn;
 rknn_context ctx=0; rknn_init(&ctx,(void*)av[1],0,0,NULL);
 rknn_tensor_attr in;memset(&in,0,sizeof(in));in.index=0;rknn_query(ctx,RKNN_QUERY_INPUT_ATTR,&in,sizeof(in));
 int W=(in.fmt==RKNN_TENSOR_NHWC)?in.dims[2]:in.dims[3];
 rknn_tensor_attr out;memset(&out,0,sizeof(out));out.index=0;rknn_query(ctx,RKNN_QUERY_OUTPUT_ATTR,&out,sizeof(out));
 int C=out.dims[1],M=out.n_elems/C;
 printf("W=%d C=%d M=%d outype=%d\n",W,C,M,out.type);
 std::vector<uint8_t> img(W*W*3);
 for(int y=0;y<W;++y)for(int x=0;x<W;++x){int sy=std::min<int>(SRC-1,(y*SRC)/W),sx=std::min<int>(SRC-1,(x*SRC)/W);memcpy(&img[(y*W+x)*3],&src[(sy*SRC+sx)*3],3);}
 // UINT8 raw BGR
 rknn_input in_;memset(&in_,0,sizeof(in_));in_.index=0;in_.type=RKNN_TENSOR_UINT8;in_.fmt=RKNN_TENSOR_NHWC;in_.size=W*W*3;in_.buf=img.data();
 rknn_inputs_set(ctx,1,&in_); rknn_run(ctx,NULL);
 std::vector<uint8_t> ob(out.size);rknn_output o;memset(&o,0,sizeof(o));o.index=0;o.want_float=0;o.buf=ob.data();o.size=out.size;o.is_prealloc=1;
 rknn_outputs_get(ctx,1,&o,NULL);
 uint16_t* P=(uint16_t*)ob.data();
 // 布局A: C*M (channel-major, P[ch*M+a])
 printf("LAYOUT C*M: cls chavg&max (top per class):\n");
 float bestA=-1e30f;int bestAc=-1;
 {float mn=1e30f,mx=-1e30f;int mna=-1;for(int a=0;a<M;++a){float v=h2f(P[(size_t)4*M+a]);if(v<mn){mn=v;mna=a;}if(v>mx)mx=v;}
  printf("  ch4(cls0) min=%.4f@a%d max=%.4f\n",mn,mna,mx);}
 // 全类别通道值域
 {float gmin=1e30f,gmax=-1e30f;int bad=0;for(int ch=4;ch<C;++ch)for(int a=0;a<M;++a){float v=h2f(P[(size_t)ch*M+a]);if(v<gmin)gmin=v;if(v>gmax)gmax=v;if(v<0||v>1)bad++;}
  printf("  ALL cls ch: min=%.4f max=%.4f outside(0,1) count=%d (0=已sigmoid概率)\n",gmin,gmax,bad);}
 for(int ch=4;ch<C;++ch){float mx=-1e30f;int arga=-1;for(int a=0;a<M;++a){float v=h2f(P[(size_t)ch*M+a]);if(v>mx){mx=v;arga=a;}}
   if(mx>bestA){bestA=mx;bestAc=ch-4;}
   if(ch<8||mx>1.0f)printf("  ch%d(cls%-3d) max=%7.3f@a%d\n",ch,ch-4,mx,arga);}
 printf("  BEST C*M: cls%d max=%.3f\n",bestAc,bestA);
 // 布局B: M*C (anchor-major, P[a*C+ch])
 printf("LAYOUT M*C: cls chavg&max:\n");
 float bestB=-1e30f;int bestBc=-1;
 for(int ch=4;ch<C;++ch){float mx=-1e30f;int arga=-1;for(int a=0;a<M;++a){float v=h2f(P[(size_t)a*C+ch]);if(v>mx){mx=v;arga=a;}}
   if(mx>bestB){bestB=mx;bestBc=ch-4;}
   if(ch<8||mx>1.0f)printf("  ch%d(cls%-3d) max=%7.3f@a%d\n",ch,ch-4,mx,arga);}
 printf("  BEST M*C: cls%d max=%.3f\n",bestBc,bestB);
 // 布局C: 前4通道是否 sigmoid 后才有意义  (在 C*M 下坐标通道分布)
 printf("C*M coord ch0-3 sample a0-5:\n");
 for(int a=0;a<6;++a){printf("  a%d c0=%.2f c1=%.2f c2=%.2f c3=%.2f\n",a,h2f(P[a]),h2f(P[M+a]),h2f(P[2*M+a]),h2f(P[3*M+a]));}
 rknn_destroy(ctx); return 0;
}