// SPDX-License-Identifier: GPL-3.0-only
// Stable persisted IDs. Retired IDs must never be reused: 1 (RIFE 4.25),
// 2 IFRNet, 3 CAIN, 4 IFRNet Large,
// 6 NVIDIA FRUC.
#ifndef PK_MODEL_CATALOG_H
#define PK_MODEL_CATALOG_H
typedef struct {
 const wchar_t *directory,*label;
 const char *name,*param_sha256,*bin_sha256;
 int padding;
 const wchar_t *basename;
 // 0 RIFE (basename.param/.bin), 3 FSR 3 (basename = notice file, code in the engine DLL), -1 retired
 int family;
} PkModelSpec;
static const PkModelSpec pk_models[]={
 {L"model",L"RIFE 4.6", "RIFE 4.6",
  "28DF14D57A225725EE5386F52EBA422488450D37C9F40800ED4F62E8BA846692",
  "F334ED2260149CE0188A6DCF049844E8B0CDD912E01CBCFB63553157D2508958",32,L"flownet",0},
 {L"",L"","Removed model","","",0,L"",-1}, // ID 1 retired; saved settings fall back to RIFE 4.6
 {L"",L"","Removed model","","",0,L"",-1}, // ID 2 IFRNet retired
 {L"",L"","Removed model","","",0,L"",-1}, // ID 3 CAIN retired
 {L"",L"","Removed model","","",0,L"",-1}, // ID 4 IFRNet Large retired
 {L"engines\\fsr3",L"FSR 3 · 2배·4배","FSR 3",
  "82CF74FC23885107C7F69249AC3F1DAC55898FF68320EB2C753984CAE6B2D78A","",0,L"LICENSE-FidelityFX.txt",3},
 {L"",L"","Removed model","","",0,L"",-1}, // ID 6 NVIDIA FRUC retired; saved settings fall back to RIFE 4.6
 // IDs 7-10 were used only by internal test builds (7/8 RIFE 4.26 9 NVIDIA VFG and 10 RIFE 4.25 lite ); never reuse.
#ifdef PK_INTERNAL_VFG
 {L"",L"","Removed model","","",0,L"",-1} /* rife426 retired (only the measured models are kept) */,
 {L"",L"","Removed model","","",0,L"",-1} /* rife426L retired (only the measured models are kept) */,
#else
 {L"",L"","Removed model","","",0,L"",-1},{L"",L"","Removed model","","",0,L"",-1},
#endif
 // 9 = NVIDIA VFG (family 4, basename = notice file). The runtime is downloaded by the user
 // from NVIDIA (engines\nvvfg\Get-NvidiaVfg.ps1) or found in a pip install; nothing from NVIDIA is shipped.
 {L"engines\\nvvfg",L"NVIDIA VFG","NVIDIA VFG",
  "7F17D52C132172CE9D69C269BD470C293F417F11E824827AEB9D71E666B4CA78","",0,L"NOTICE-NVIDIA-VFG.txt",4},
#ifdef PK_INTERNAL_VFG
 // retired: 10 = RIFE 4.25 lite (same ID and padding 128 as the build).
 {L"",L"","Removed model","","",0,L"",-1} /* rife425lite retired (only the measured models are kept) */,
#else
 {L"",L"","Removed model","","",0,L"",-1},
#endif
 // RIFE 4.15 lite (TNTwise/rife-ncnn-vulkan 13338e38, MIT, unmodified). Same ID as the build.
 // game recording 60->120: 28.68 vs 28.26 dB (4.6) at FHD, 28.95/28.84 vs 28.31/28.22 dB at QHD/UHD; ~1.1-1.25x slower.
 {L"rife415lite",L"RIFE 4.15 lite","RIFE 4.15 lite",
  "18BC572AA0CD78AB7242E60840CDDE0FEAE37DD319D2A7B108989D83CC6479F1",
  "700F6D15FC365DFD99A8FB7B4F9986708D4E9FB89844F3DDC4B56E7F30D16CDA",32,L"flownet",0}
 // 12 = NVIDIA DLSS Frame Generation (family 5, public NGX DLSSG API offscreen, pk_dlssg.dll).
 // nvngx_dlssg.dll is bundled in the installer.
#ifdef PK_INTERNAL_DLSSG
 ,{L"engines\\dlssg",L"NVIDIA DLSS FG","DLSS FG",
  "7FE316598E956D750B59A1212BA24C5DEC8ABC40BB90C5D67E28F467362017CC","",0,L"NOTICE-NVIDIA-DLSSG.txt",5}
#else
 ,{L"",L"","Removed model","","",0,L"",-1} // 12: DLSS FG left out of this build
#endif
 // Retired: more TNTwise rife-ncnn-vulkan 13338e38 models (MIT). 13-16 keep the -build IDs;
 // RIFE 4.17 (test ID 12, now DLSS FG) moves to 17; 18 is kept for AMD FSR (ffx-api DX12); 19 = RIFE 4.25.
 ,{L"",L"","Removed model","","",0,L"",-1} /* rife418 retired (only the measured models are kept) */
 ,{L"",L"","Removed model","","",0,L"",-1} /* rife422 retired (only the measured models are kept) */
 ,{L"",L"","Removed model","","",0,L"",-1} /* rife422lite retired (only the measured models are kept) */
 ,{L"",L"","Removed model","","",0,L"",-1} /* rife424 retired (only the measured models are kept) */
 ,{L"",L"","Removed model","","",0,L"",-1} /* rife417 retired (only the measured models are kept) */
 // 18 = AMD FidelityFX SDK 2.4.0 frame generation DLL (family 6, D3D12 offscreen, pk_amdfg.dll + AMD signed DLLs).
 ,{L"engines\\amdfg",L"AMD FSR FG","AMD FSR FG",
  "6D931769BDF61235B62B9428FA288CBCEE71E4FBE5AEDF1D6A7315B758F14B1E","",0,L"NOTICE-AMD-FG.txt",6}
 ,{L"",L"","Removed model","","",0,L"",-1} /* rife425 retired (only the measured models are kept) */
};
#define PK_MODEL_COUNT 20
// menu order: RIFE 4.6, RIFE 4.15 lite, FSR 3, AMD FSR FG, NVIDIA VFG, DLSS FG.
#ifdef PK_INTERNAL_DLSSG
static const int pk_model_order[]={0,11,5,18,9,12};
#else
static const int pk_model_order[]={0,11,5,18,9};
#endif
#define PK_MODEL_ORDER_COUNT ((int)(sizeof pk_model_order/sizeof pk_model_order[0]))
static int pk_model_valid(int id){return id>=0&&id<PK_MODEL_COUNT&&pk_models[id].family>=0;}
// FSR 3 interpolates the midpoint only; 4x halves twice, 3x is refused.
static int pk_model_rate_supported(int id,int sn,int sd,int tn,int td){
 if(!pk_model_valid(id)||sn<=0||sd<=0||tn<=0||td<=0)return 0;
 long long t=(long long)tn*sd,s=(long long)sn*td;
 if(pk_models[id].family==3)return t==2*s||t==4*s; // 4x = midpoint twice
 if(pk_models[id].family==6)return t==2*s; // AMD FSR FG DLL: midpoint only
 if(pk_models[id].family==5)return t==2*s||t==3*s||t==4*s; // DLSS FG: 2x, 3x/4x multi-frame (RTX 50)
 return 1;
}
#endif
