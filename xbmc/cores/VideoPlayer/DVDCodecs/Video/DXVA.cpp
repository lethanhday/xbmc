/*
 *      Copyright (C) 2005-2013 Team XBMC
 *      http://kodi.tv
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

// setting that here because otherwise SampleFormat is defined to AVSampleFormat
// which we don't use here
#define FF_API_OLD_SAMPLE_FMT 0

#include "DXVA.h"
#include "cores/VideoPlayer/DVDCodecs/DVDCodecUtils.h"
#include "cores/VideoPlayer/DVDCodecs/DVDFactoryCodec.h"
#include "cores/VideoPlayer/Process/ProcessInfo.h"
#include "cores/VideoPlayer/VideoRenderers/RenderManager.h"
#include "rendering/dx/DeviceResources.h"
#include "rendering/dx/RenderContext.h"
#include "ServiceBroker.h"
#include "settings/AdvancedSettings.h"
#include "settings/Settings.h"
#include "utils/Log.h"
#include "utils/StringUtils.h"
#include "utils/SystemInfo.h"

#include <dxva.h>
#include <d3d11.h>
#include <Initguid.h>
#include <windows.h>

using namespace DXVA;
using namespace Microsoft::WRL;

DEFINE_GUID(DXVADDI_Intel_ModeH264_A,      0x604F8E64,0x4951,0x4c54,0x88,0xFE,0xAB,0xD2,0x5C,0x15,0xB3,0xD6);
DEFINE_GUID(DXVADDI_Intel_ModeH264_C,      0x604F8E66,0x4951,0x4c54,0x88,0xFE,0xAB,0xD2,0x5C,0x15,0xB3,0xD6);
DEFINE_GUID(DXVADDI_Intel_ModeH264_E,      0x604F8E68,0x4951,0x4c54,0x88,0xFE,0xAB,0xD2,0x5C,0x15,0xB3,0xD6);
DEFINE_GUID(DXVADDI_Intel_ModeVC1_E,       0xBCC5DB6D,0xA2B6,0x4AF0,0xAC,0xE4,0xAD,0xB1,0xF7,0x87,0xBC,0x89);
DEFINE_GUID(DXVA_ModeH264_VLD_NoFGT_Flash, 0x4245F676,0x2BBC,0x4166,0xa0,0xBB,0x54,0xE7,0xB8,0x49,0xC3,0x80);
DEFINE_GUID(DXVA_Intel_VC1_ClearVideo_2,   0xE07EC519,0xE651,0x4CD6,0xAC,0x84,0x13,0x70,0xCC,0xEE,0xC8,0x51);

// redefine DXVA_NoEncrypt with other macro, solves unresolved external symbol linker error
#ifdef DXVA_NoEncrypt 
#undef DXVA_NoEncrypt
#endif
DEFINE_GUID(DXVA_NoEncrypt, 0x1b81beD0, 0xa0c7, 0x11d3, 0xb9, 0x84, 0x00, 0xc0, 0x4f, 0x2e, 0x73, 0xc5);

static const int PROFILES_MPEG2_SIMPLE[] = { FF_PROFILE_MPEG2_SIMPLE, 
                                             FF_PROFILE_UNKNOWN };
static const int PROFILES_MPEG2_MAIN[]   = { FF_PROFILE_MPEG2_SIMPLE,
                                             FF_PROFILE_MPEG2_MAIN, 
                                             FF_PROFILE_UNKNOWN };
static const int PROFILES_H264_HIGH[]    = { FF_PROFILE_H264_BASELINE,
                                             FF_PROFILE_H264_CONSTRAINED_BASELINE,
                                             FF_PROFILE_H264_MAIN,
                                             FF_PROFILE_H264_HIGH, 
                                             FF_PROFILE_UNKNOWN };
static const int PROFILES_HEVC_MAIN[]    = { FF_PROFILE_HEVC_MAIN, 
                                             FF_PROFILE_UNKNOWN };
static const int PROFILES_HEVC_MAIN10[]  = { FF_PROFILE_HEVC_MAIN,
                                             FF_PROFILE_HEVC_MAIN_10, 
                                             FF_PROFILE_UNKNOWN };
static const int PROFILES_VP9_0[]        = { FF_PROFILE_VP9_0, 
                                             FF_PROFILE_UNKNOWN };
static const int PROFILES_VP9_10_2[]     = { FF_PROFILE_VP9_2, 
                                             FF_PROFILE_UNKNOWN };

typedef struct {
    const char   *name;
    const GUID   *guid;
    int          codec;
    const int*   profiles;
} dxva2_mode_t;

/* XXX Prefered modes must come first */
static const std::vector<dxva2_mode_t> dxva2_modes = {
    { "MPEG2 variable-length decoder",                                              &D3D11_DECODER_PROFILE_MPEG2_VLD,     AV_CODEC_ID_MPEG2VIDEO, PROFILES_MPEG2_MAIN },
    { "MPEG1/2 variable-length decoder",                                            &D3D11_DECODER_PROFILE_MPEG2and1_VLD, AV_CODEC_ID_MPEG2VIDEO, PROFILES_MPEG2_MAIN },
    { "MPEG2 motion compensation",                                                  &D3D11_DECODER_PROFILE_MPEG2_MOCOMP,  0, nullptr },
    { "MPEG2 inverse discrete cosine transform",                                    &D3D11_DECODER_PROFILE_MPEG2_IDCT,    0, nullptr},

    { "MPEG-1 variable-length decoder",                                             &D3D11_DECODER_PROFILE_MPEG1_VLD,     0, nullptr },

    { "H.264 variable-length decoder, film grain technology",                       &D3D11_DECODER_PROFILE_H264_VLD_FGT,              AV_CODEC_ID_H264, PROFILES_H264_HIGH },
    { "H.264 variable-length decoder, no film grain technology (Intel ClearVideo)", &DXVADDI_Intel_ModeH264_E,                        AV_CODEC_ID_H264, PROFILES_H264_HIGH },
    { "H.264 variable-length decoder, no film grain technology",                    &D3D11_DECODER_PROFILE_H264_VLD_NOFGT,            AV_CODEC_ID_H264, PROFILES_H264_HIGH },
    { "H.264 variable-length decoder, no film grain technology, FMO/ASO",           &D3D11_DECODER_PROFILE_H264_VLD_WITHFMOASO_NOFGT, AV_CODEC_ID_H264, PROFILES_H264_HIGH },
    { "H.264 variable-length decoder, no film grain technology, Flash",             &DXVA_ModeH264_VLD_NoFGT_Flash,                   AV_CODEC_ID_H264, PROFILES_H264_HIGH },

    { "H.264 inverse discrete cosine transform, film grain technology",             &D3D11_DECODER_PROFILE_H264_IDCT_FGT,     0, nullptr },
    { "H.264 inverse discrete cosine transform, no film grain technology",          &D3D11_DECODER_PROFILE_H264_IDCT_NOFGT,   0, nullptr },
    { "H.264 inverse discrete cosine transform, no film grain technology (Intel)",  &DXVADDI_Intel_ModeH264_C,                0, nullptr },

    { "H.264 motion compensation, film grain technology",                           &D3D11_DECODER_PROFILE_H264_MOCOMP_FGT,   0, nullptr },
    { "H.264 motion compensation, no film grain technology",                        &D3D11_DECODER_PROFILE_H264_MOCOMP_NOFGT, 0, nullptr },
    { "H.264 motion compensation, no film grain technology (Intel)",                &DXVADDI_Intel_ModeH264_A,                0, nullptr },

    { "H.264 stereo high profile, mbs flag set",                                    &D3D11_DECODER_PROFILE_H264_VLD_STEREO_PROGRESSIVE_NOFGT, 0, nullptr },
    { "H.264 stereo high profile",                                                  &D3D11_DECODER_PROFILE_H264_VLD_STEREO_NOFGT,             0, nullptr },
    { "H.264 multiview high profile",                                               &D3D11_DECODER_PROFILE_H264_VLD_MULTIVIEW_NOFGT,          0, nullptr },

    { "Windows Media Video 8 motion compensation",                                  &D3D11_DECODER_PROFILE_WMV8_MOCOMP,   0, nullptr },
    { "Windows Media Video 8 post processing",                                      &D3D11_DECODER_PROFILE_WMV8_POSTPROC, 0, nullptr },

    { "Windows Media Video 9 inverse discrete cosine transform",                    &D3D11_DECODER_PROFILE_WMV9_IDCT,     0, nullptr },
    { "Windows Media Video 9 motion compensation",                                  &D3D11_DECODER_PROFILE_WMV9_MOCOMP,   0, nullptr },
    { "Windows Media Video 9 post processing",                                      &D3D11_DECODER_PROFILE_WMV9_POSTPROC, 0, nullptr },

    { "VC-1 variable-length decoder",                                               &D3D11_DECODER_PROFILE_VC1_VLD,      AV_CODEC_ID_VC1, nullptr },
    { "VC-1 variable-length decoder",                                               &D3D11_DECODER_PROFILE_VC1_VLD,      AV_CODEC_ID_WMV3, nullptr },
    { "VC-1 variable-length decoder 2010",                                          &D3D11_DECODER_PROFILE_VC1_D2010,    AV_CODEC_ID_VC1, nullptr },
    { "VC-1 variable-length decoder 2010",                                          &D3D11_DECODER_PROFILE_VC1_D2010,    AV_CODEC_ID_WMV3, nullptr },
    { "VC-1 variable-length decoder 2 (Intel)",                                     &DXVA_Intel_VC1_ClearVideo_2,        0, nullptr },
    { "VC-1 variable-length decoder (Intel)",                                       &DXVADDI_Intel_ModeVC1_E,            0, nullptr },

    { "VC-1 inverse discrete cosine transform",                                     &D3D11_DECODER_PROFILE_VC1_IDCT,     0, nullptr },
    { "VC-1 motion compensation",                                                   &D3D11_DECODER_PROFILE_VC1_MOCOMP,   0, nullptr },
    { "VC-1 post processing",                                                       &D3D11_DECODER_PROFILE_VC1_POSTPROC, 0, nullptr },

    { "HEVC variable-length decoder, main",                                         &D3D11_DECODER_PROFILE_HEVC_VLD_MAIN,   AV_CODEC_ID_HEVC, PROFILES_HEVC_MAIN },
    { "HEVC variable-length decoder, main10",                                       &D3D11_DECODER_PROFILE_HEVC_VLD_MAIN10, AV_CODEC_ID_HEVC, PROFILES_HEVC_MAIN10 },

    { "VP9 variable-length decoder, Profile 0",                                     &D3D11_DECODER_PROFILE_VP9_VLD_PROFILE0,       AV_CODEC_ID_VP9, PROFILES_VP9_0 },
    { "VP9 variable-length decoder, 10bit, profile 2",                              &D3D11_DECODER_PROFILE_VP9_VLD_10BIT_PROFILE2, AV_CODEC_ID_VP9, PROFILES_VP9_10_2 },
};

// Prefered targets must be first
static const DXGI_FORMAT render_targets_dxgi[] = {
  DXGI_FORMAT_NV12,
  DXGI_FORMAT_P010,
  DXGI_FORMAT_P016,
  DXGI_FORMAT_UNKNOWN
};

// List of PCI Device ID of ATI cards with UVD or UVD+ decoding block.
static DWORD UVDDeviceID [] = {
  0x95C0, // ATI Radeon HD 3400 Series (and others)
  0x95C5, // ATI Radeon HD 3400 Series (and others)
  0x95C4, // ATI Radeon HD 3400 Series (and others)
  0x94C3, // ATI Radeon HD 3410
  0x9589, // ATI Radeon HD 3600 Series (and others)
  0x9598, // ATI Radeon HD 3600 Series (and others)
  0x9591, // ATI Radeon HD 3600 Series (and others)
  0x9501, // ATI Radeon HD 3800 Series (and others)
  0x9505, // ATI Radeon HD 3800 Series (and others)
  0x9507, // ATI Radeon HD 3830
  0x9513, // ATI Radeon HD 3850 X2
  0x950F, // ATI Radeon HD 3850 X2
  0x0000
};

// List of PCI Device ID of nVidia cards with the macroblock width issue. More or less the VP3 block.
// Per NVIDIA Accelerated Linux Graphics Driver, Appendix A Supported NVIDIA GPU Products, cards with note 1.
static DWORD VP3DeviceID [] = {
  0x06E0, // GeForce 9300 GE
  0x06E1, // GeForce 9300 GS
  0x06E2, // GeForce 8400
  0x06E4, // GeForce 8400 GS
  0x06E5, // GeForce 9300M GS
  0x06E6, // GeForce G100
  0x06E8, // GeForce 9200M GS
  0x06E9, // GeForce 9300M GS
  0x06EC, // GeForce G 105M
  0x06EF, // GeForce G 103M
  0x06F1, // GeForce G105M
  0x0844, // GeForce 9100M G
  0x0845, // GeForce 8200M G
  0x0846, // GeForce 9200
  0x0847, // GeForce 9100
  0x0848, // GeForce 8300
  0x0849, // GeForce 8200
  0x084A, // nForce 730a
  0x084B, // GeForce 9200
  0x084C, // nForce 980a/780a SLI
  0x084D, // nForce 750a SLI
  0x0860, // GeForce 9400
  0x0861, // GeForce 9400
  0x0862, // GeForce 9400M G
  0x0863, // GeForce 9400M
  0x0864, // GeForce 9300
  0x0865, // ION
  0x0866, // GeForce 9400M G
  0x0867, // GeForce 9400
  0x0868, // nForce 760i SLI
  0x086A, // GeForce 9400
  0x086C, // GeForce 9300 / nForce 730i
  0x086D, // GeForce 9200
  0x086E, // GeForce 9100M G
  0x086F, // GeForce 8200M G
  0x0870, // GeForce 9400M
  0x0871, // GeForce 9200
  0x0872, // GeForce G102M
  0x0873, // GeForce G102M
  0x0874, // ION
  0x0876, // ION
  0x087A, // GeForce 9400
  0x087D, // ION
  0x087E, // ION LE
  0x087F, // ION LE
  0x0000
};

static std::string GUIDToString(const GUID& guid)
{
  std::string buffer = StringUtils::Format("%08X-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x"
              , guid.Data1, guid.Data2, guid.Data3
              , guid.Data4[0], guid.Data4[1]
              , guid.Data4[2], guid.Data4[3], guid.Data4[4]
              , guid.Data4[5], guid.Data4[6], guid.Data4[7]);
  return buffer;
}

static const dxva2_mode_t *dxva2_find_mode(const GUID *guid)
{
  for (const dxva2_mode_t& mode : dxva2_modes)
  {
    if (IsEqualGUID(*mode.guid, *guid))
      return &mode;
  }
  return nullptr;
}

//-----------------------------------------------------------------------------
// DXVA Context
//-----------------------------------------------------------------------------

CDXVAContext *CDXVAContext::m_context = nullptr;
CCriticalSection CDXVAContext::m_section;

CDXVAContext::CDXVAContext()
  : m_vcontext(nullptr)
  , m_service(nullptr)
  , m_refCount(0)
  , m_input_count(0)
  , m_input_list(nullptr)
  , m_atiWorkaround(false)
{
  m_context = nullptr;
}

void CDXVAContext::Release(CDecoder *decoder)
{
  CSingleLock lock(m_section);
  auto it = find(m_decoders.begin(), m_decoders.end(), decoder);
  if (it != m_decoders.end())
    m_decoders.erase(it);
  m_refCount--;
  if (m_refCount <= 0)
  {
    Close();
    delete this;
    m_context = nullptr;
  }
}

void CDXVAContext::Close()
{
  CLog::LogFunction(LOGNOTICE, "DXVA", "closing decoder context.");
  DestroyContext();
}

bool CDXVAContext::EnsureContext(CDXVAContext **ctx, CDecoder *decoder)
{
  CSingleLock lock(m_section);
  if (m_context)
  {
    m_context->m_refCount++;
    *ctx = m_context;
    if (!m_context->IsValidDecoder(decoder))
      m_context->m_decoders.push_back(decoder);
    return true;
  }
  m_context = new CDXVAContext();
  *ctx = m_context;
  {
    if (!m_context->CreateContext())
    {
      delete m_context;
      m_context = nullptr;
      *ctx = nullptr;
      return false;
    }
  }
  m_context->m_refCount++;

  if (!m_context->IsValidDecoder(decoder))
    m_context->m_decoders.push_back(decoder);

  *ctx = m_context;
  return true;
}

bool CDXVAContext::CreateContext()
{
  ComPtr<ID3D11Device> pD3DDevice(DX::DeviceResources::Get()->GetD3DDevice());
  ComPtr<ID3D11DeviceContext> pD3DDeviceContext(DX::DeviceResources::Get()->GetImmediateContext());

  if (FAILED(pD3DDevice.As(&m_service)) || FAILED(pD3DDeviceContext.As(&m_vcontext)))
  {
    CLog::LogF(LOGWARNING, "failed to get Video Device and Context.");
    return false;
  }

  QueryCaps();

  // Some older Ati devices can only open a single decoder at a given time
  std::string renderer =  DX::Windowing().GetRenderRenderer();
  if (renderer.find("Radeon HD 2") != std::string::npos ||
      renderer.find("Radeon HD 3") != std::string::npos ||
      renderer.find("Radeon HD 4") != std::string::npos ||
      renderer.find("Radeon HD 5") != std::string::npos)
  {
    m_atiWorkaround = true;
  }

  return true;
}

void CDXVAContext::DestroyContext()
{
  delete[] m_input_list;
  m_service = nullptr;
  m_vcontext = nullptr;
}

void CDXVAContext::QueryCaps()
{
  m_input_count = m_service->GetVideoDecoderProfileCount();
  
  m_input_list = new GUID[m_input_count];
  for (unsigned i = 0; i < m_input_count; i++)
  {
    if (FAILED(m_service->GetVideoDecoderProfile(i, &m_input_list[i])))
    {
      CLog::LogFunction(LOGNOTICE, "DXVA", "failed getting device guids.");
      return;
    }
    const dxva2_mode_t *mode = dxva2_find_mode(&m_input_list[i]);
    if (mode)
      CLog::LogFunction(LOGDEBUG, "DXVA", "supports '%s'.", mode->name);
    else
      CLog::LogFunction(LOGDEBUG, "DXVA", "supports %s.", GUIDToString(m_input_list[i]).c_str());
  }
}

bool CDXVAContext::GetFormatAndConfig(AVCodecContext* avctx, D3D11_VIDEO_DECODER_DESC &format, D3D11_VIDEO_DECODER_CONFIG &config) const
{
  format.OutputFormat = DXGI_FORMAT_UNKNOWN;

  // iterate through our predefined dxva modes and find the first matching for desired codec
  // once we found a mode, get a target we support in render_targets_dxgi DXGI_FORMAT_UNKNOWN
  for (const dxva2_mode_t& mode : dxva2_modes)
  {
    if (mode.codec != avctx->codec_id)
      continue;

    bool supported = false;
    for (unsigned i = 0; i < m_input_count && !supported; i++)
    {
      supported = IsEqualGUID(m_input_list[i], *mode.guid) != 0;
    }
    if (supported)
    {
      // check profiles
      supported = false;
      if (mode.profiles == nullptr)
        supported = true;
      else if (avctx->profile == FF_PROFILE_UNKNOWN)
        supported = true;
      else for (const int *pProfile = &mode.profiles[0]; *pProfile != FF_PROFILE_UNKNOWN; ++pProfile)
      {
        if (*pProfile == avctx->profile)
        {
          supported = true;
          break;
        }
      }
      if (!supported)
          CLog::LogFunction(LOGDEBUG, "DXVA", "Unsupported profile %d for %s.", avctx->profile, mode.name);
    }
    if (!supported)
      continue;

    CLog::LogFunction(LOGDEBUG, "DXVA", "trying '%s'.", mode.name);
    for (unsigned j = 0; render_targets_dxgi[j]; ++j)
    {
      bool bHighBits = (avctx->codec_id == AV_CODEC_ID_HEVC && (avctx->sw_pix_fmt == AV_PIX_FMT_YUV420P10 || avctx->profile == FF_PROFILE_HEVC_MAIN_10))
                    || (avctx->codec_id == AV_CODEC_ID_VP9 && (avctx->sw_pix_fmt == AV_PIX_FMT_YUV420P10 || avctx->profile == FF_PROFILE_VP9_2));
      if (bHighBits && render_targets_dxgi[j] < DXGI_FORMAT_P010)
        continue;

      BOOL format_supported = FALSE;
      HRESULT res = m_service->CheckVideoDecoderFormat(mode.guid, render_targets_dxgi[j], &format_supported);
      if (FAILED(res) || !format_supported)
      {
        CLog::LogFunction(LOGNOTICE, "DXVA", "Ouput format %d is not supported by '%s'", render_targets_dxgi[j], mode.name);
        continue;
      }

      // check decoder config
      D3D11_VIDEO_DECODER_DESC checkFormat =
      {
        *mode.guid,
        avctx->coded_width,
        avctx->coded_height,
        render_targets_dxgi[j]
      };
      if (!GetConfig(checkFormat, config))
        continue;

      // config is found, update decoder description
      format.Guid = *mode.guid;
      format.OutputFormat = render_targets_dxgi[j];
      format.SampleWidth = avctx->coded_width;
      format.SampleHeight = avctx->coded_height;
      return true;
    }
  }

  return false;
}

bool CDXVAContext::GetConfig(const D3D11_VIDEO_DECODER_DESC &format, D3D11_VIDEO_DECODER_CONFIG &config) const
{
  // find what decode configs are available
  UINT cfg_count = 0;
  HRESULT res = m_service->GetVideoDecoderConfigCount(&format, &cfg_count);

  if (FAILED(res))
  {
    CLog::LogF(LOGNOTICE, "failed getting decoder configuration count.");
    return false;
  }
  if (!cfg_count)
  {
    CLog::LogF(LOGNOTICE, "no decoder configuration possible for %dx%d (%d).", format.SampleWidth, format.SampleHeight, format.OutputFormat);
    return false;
  }

  config = { 0 };
  unsigned bitstream = 2; // ConfigBitstreamRaw = 2 is required for Poulsbo and handles skipping better with nVidia
  for (unsigned i = 0; i< cfg_count; i++)
  {
    D3D11_VIDEO_DECODER_CONFIG pConfig = {0};
    if (FAILED(m_service->GetVideoDecoderConfig(&format, i, &pConfig)))
    {
      CLog::LogF(LOGNOTICE, "failed getting decoder configuration.");
      return false;
    }

    CLog::LogFunction(LOGDEBUG, "DXVA", "config %d: bitstream type %d%s.", i,
      pConfig.ConfigBitstreamRaw,
      IsEqualGUID(pConfig.guidConfigBitstreamEncryption, DXVA_NoEncrypt) ? "" : ", encrypted");

    // select first available
    if (config.ConfigBitstreamRaw == 0 && pConfig.ConfigBitstreamRaw != 0)
      config = pConfig;
    // override with preferred if found
    if (config.ConfigBitstreamRaw != bitstream && pConfig.ConfigBitstreamRaw == bitstream)
      config = pConfig;
  }

  if (!config.ConfigBitstreamRaw)
  {
    CLog::LogFunction(LOGDEBUG, "DXVA", "failed to find a raw input bitstream.");
    return false;
  }

  return true;
}

bool CDXVAContext::CreateSurfaces(const D3D11_VIDEO_DECODER_DESC &format, const uint32_t count, const uint32_t alignment
                                , ID3D11VideoDecoderOutputView **surfaces) const
{
  HRESULT hr = S_OK;
  ComPtr<ID3D11Device> pD3DDevice(DX::DeviceResources::Get()->GetD3DDevice());
  ComPtr<ID3D11DeviceContext1> pD3DDeviceContext(DX::DeviceResources::Get()->GetImmediateContext());

  unsigned bindFlags = D3D11_BIND_DECODER;

  if (DX::Windowing().IsFormatSupport(format.OutputFormat, D3D11_FORMAT_SUPPORT_SHADER_SAMPLE))
    bindFlags |= D3D11_BIND_SHADER_RESOURCE;

  CD3D11_TEXTURE2D_DESC texDesc(format.OutputFormat, 
                                FFALIGN(format.SampleWidth, alignment), 
                                FFALIGN(format.SampleHeight, alignment), 
                                count, 1, bindFlags);

  CLog::LogFunction(LOGDEBUG, "DXVA", "allocating %d surfaces with format %d.", count, format.OutputFormat);

  ComPtr<ID3D11Texture2D> texture;
  if (FAILED(pD3DDevice->CreateTexture2D(&texDesc, NULL, texture.GetAddressOf())))
  {
    CLog::LogF(LOGERROR, "failed creating decoder texture array.");
    return false;
  }

  D3D11_VIDEO_DECODER_OUTPUT_VIEW_DESC vdovDesc;
  vdovDesc.DecodeProfile = format.Guid;
  vdovDesc.Texture2D.ArraySlice = 0;
  vdovDesc.ViewDimension = D3D11_VDOV_DIMENSION_TEXTURE2D;
  float clearColor[] = { 0.0625f, 0.5f, 0.5f, 1.0f }; // black color in YUV

  size_t i;
  for (i = 0; i < count; ++i)
  {
    vdovDesc.Texture2D.ArraySlice = D3D11CalcSubresource(0, i, texDesc.MipLevels);
    hr = m_service->CreateVideoDecoderOutputView(texture.Get(), &vdovDesc, &surfaces[i]);
    if (FAILED(hr))
    {
      CLog::LogF(LOGERROR, "failed creating surfaces.");
      break;
    }
    pD3DDeviceContext->ClearView(surfaces[i], clearColor, nullptr, 0);
  }

  if (FAILED(hr))
  {
    for (size_t j = 0; j < i && surfaces[j]; ++j)
    {
      surfaces[j]->Release();
      surfaces[j] = nullptr;
    };
  }

  return SUCCEEDED(hr);
}

bool CDXVAContext::CreateDecoder(const D3D11_VIDEO_DECODER_DESC &format, const D3D11_VIDEO_DECODER_CONFIG &config
                               , ID3D11VideoDecoder **decoder, ID3D11VideoContext **context)
{
  CSingleLock lock(m_section);

  int retry = 0;
  while (retry < 2)
  {
    if (!m_atiWorkaround || retry > 0)
    {
      ComPtr<ID3D11VideoDecoder> pDecoder;
      HRESULT res = m_service->CreateVideoDecoder(&format, &config, pDecoder.GetAddressOf());
      if (!FAILED(res))
      {
        *decoder = pDecoder.Detach();
        m_vcontext.CopyTo(context);
        return true;
      }
    }

    if (retry == 0)
    {
      CLog::LogF(LOGNOTICE, "hw may not support multiple decoders, releasing existing ones.");
      for (auto it = m_decoders.begin(); it != m_decoders.end(); ++it)
      {
        (*it)->CloseDXVADecoder();
      }
    }
    retry++;
  }

  CLog::LogF(LOGERROR, "failed creating decoder.");
  return false;
}

bool CDXVAContext::IsValidDecoder(CDecoder *decoder)
{
  auto it = find(m_decoders.begin(), m_decoders.end(), decoder);
  if (it != m_decoders.end())
    return true;
  return false;
}

//-----------------------------------------------------------------------------
// CDXVAOutputBuffer
//-----------------------------------------------------------------------------

static DXGI_FORMAT plane_formats[][2] =
{
  { DXGI_FORMAT_R8_UNORM,  DXGI_FORMAT_R8G8_UNORM }, // NV12
  { DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16G16_UNORM }, // P010
  { DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16G16_UNORM }  // P016
};

CDXVAOutputBuffer::CDXVAOutputBuffer(int id) : CVideoBuffer(id)
{
  m_pixFormat = AV_PIX_FMT_D3D11VA_VLD;
  m_pFrame = av_frame_alloc();
}

CDXVAOutputBuffer::~CDXVAOutputBuffer()
{
  av_frame_free(&m_pFrame);
}

ID3D11View* CDXVAOutputBuffer::GetSRV(unsigned idx)
{
  if (!DX::Windowing().IsFormatSupport(format, D3D11_FORMAT_SUPPORT_SHADER_SAMPLE))
    return nullptr;

  if (planes[idx])
    return planes[idx].Get();

  ComPtr<ID3D11Resource> pResource;
  D3D11_VIDEO_DECODER_OUTPUT_VIEW_DESC vpivd;

  ComPtr<ID3D11VideoDecoderOutputView> pView(reinterpret_cast<ID3D11VideoDecoderOutputView*>(view));
  pView->GetDesc(&vpivd);
  pView->GetResource(pResource.GetAddressOf());

  DXGI_FORMAT plane_format = plane_formats[format - DXGI_FORMAT_NV12][idx];
  CD3D11_SHADER_RESOURCE_VIEW_DESC srvDesc(D3D11_SRV_DIMENSION_TEXTURE2DARRAY, plane_format,
    0, 1, vpivd.Texture2D.ArraySlice, 1);

  ComPtr<ID3D11Device> pD3DDevice(DX::DeviceResources::Get()->GetD3DDevice());
  HRESULT hr = pD3DDevice->CreateShaderResourceView(pResource.Get(), &srvDesc, planes[idx].ReleaseAndGetAddressOf());
  if (FAILED(hr))
    CLog::LogF(LOGERROR, "unable to create SRV for decoder surface (%d)", plane_format);

  return planes[idx].Get();
}

void CDXVAOutputBuffer::SetRef(AVFrame* frame)
{
  av_frame_unref(m_pFrame);
  av_frame_ref(m_pFrame, frame);
  view = reinterpret_cast<ID3D11View*>(frame->data[3]);
}

void CDXVAOutputBuffer::Unref()
{
  view = nullptr;
  planes[0] = nullptr;
  planes[1] = nullptr;
  av_frame_unref(m_pFrame);
}

//-----------------------------------------------------------------------------
// DXVA CDXVABufferPool
//-----------------------------------------------------------------------------

CDXVABufferPool::CDXVABufferPool()
{
}

CDXVABufferPool::~CDXVABufferPool()
{
  CLog::LogF(LOGDEBUG, "destructing buffer pool.");
  Reset();
}

CVideoBuffer* CDXVABufferPool::Get()
{
  CSingleLock lock(m_section);

  CDXVAOutputBuffer* retPic;
  if (!m_freeOut.empty())
  {
    size_t idx = m_freeOut.front();
    m_freeOut.pop_front();
    retPic = m_out[idx];
  }
  else
  {
    size_t idx = m_out.size();
    retPic = new CDXVAOutputBuffer(idx);
    m_out.push_back(retPic);
  }

  retPic->Acquire(GetPtr());
  return retPic;
}

void CDXVABufferPool::Return(int id)
{
  CSingleLock lock(m_section);

  auto buf = m_out[id];
  buf->Unref();

  m_freeOut.push_back(id);
}

void CDXVABufferPool::AddView(ID3D11View* view)
{
  CSingleLock lock(m_section);
  size_t idx = m_views.size();
  m_views.push_back(view);
  m_freeViews.push_back(idx);
}

void CDXVABufferPool::ReturnView(ID3D11View* surf)
{
  CSingleLock lock(m_section);

  auto it = std::find(m_views.begin(), m_views.end(), surf);
  if (it == m_views.end())
    return;

  size_t idx = it - m_views.begin();
  m_freeViews.push_back(idx);
}

bool CDXVABufferPool::IsValid(ID3D11View* surf)
{
  CSingleLock lock(m_section);
  auto it = std::find(m_views.begin(), m_views.end(), surf);
  return it != m_views.end();
}

ID3D11View* CDXVABufferPool::GetView()
{
  CSingleLock lock(m_section);

  if (!m_freeViews.empty())
  {
    size_t idx = m_freeViews.front();
    m_freeViews.pop_front();

    auto view = m_views[idx];
    return view;
  }
  return nullptr;
}

void CDXVABufferPool::Reset()
{
  CSingleLock lock(m_section);

  for (auto v : m_views)
    if (v) v->Release();

  for (auto buf : m_out)
    delete buf;

  m_views.clear();
  m_freeViews.clear();
  m_out.clear();
  m_freeOut.clear();
}

size_t CDXVABufferPool::Size()
{
  CSingleLock lock(m_section);
  return m_views.size();
}

bool CDXVABufferPool::HasFree()
{
  CSingleLock lock(m_section);
  return !m_freeViews.empty();
}

bool CDXVABufferPool::HasRefs()
{
  CSingleLock lock(m_section);
  // out buffers hold views
  size_t buffRefs = m_out.size() - m_freeOut.size();
  // ffmpeg refs = total - free - out refs
  return m_freeViews.size() != m_views.size() - buffRefs;
}

//-----------------------------------------------------------------------------
// DXVA Decoder
//-----------------------------------------------------------------------------

IHardwareDecoder* CDecoder::Create(CDVDStreamInfo &hint, CProcessInfo &processInfo, AVPixelFormat fmt)
{
  if (DXVA::CDecoder::Supports(fmt) && CServiceBroker::GetSettings().GetBool(CSettings::SETTING_VIDEOPLAYER_USEDXVA2))
    return new CDecoder(processInfo);

  return nullptr;
}

bool CDecoder::Register()
{
  CDVDFactoryCodec::RegisterHWAccel("dxva", CDecoder::Create);
  return true;
}

CDecoder::CDecoder(CProcessInfo& processInfo)
  : m_state(DXVA_OPEN)
  , m_refs(0)
  , m_device(nullptr)
  , m_decoder(nullptr)
  , m_vcontext(nullptr)
  , m_format()
  , m_videoBuffer(nullptr)
  , m_dxva_context(nullptr)
  , m_shared(0)
  , m_surface_alignment(16)
  , m_event(true)
  , m_processInfo(processInfo)
{
  m_event.Set();
  m_context          = static_cast<AVD3D11VAContext*>(calloc(1, sizeof(AVD3D11VAContext)));
  m_context->cfg     = reinterpret_cast<D3D11_VIDEO_DECODER_CONFIG*>(calloc(1, sizeof(D3D11_VIDEO_DECODER_CONFIG)));
  m_context->surface = reinterpret_cast<ID3D11VideoDecoderOutputView**>(calloc(32, sizeof(ID3D11VideoDecoderOutputView*)));
  m_bufferPool.reset();
  DX::Windowing().Register(this);
}

CDecoder::~CDecoder()
{
  CLog::LogF(LOGDEBUG, "destructing decoder, %p.", this);
  DX::Windowing().Unregister(this);
  Close();
  free(m_context->surface);
  free(m_context->cfg);
  free(m_context);
}

long CDecoder::Release()
{
  // if ffmpeg holds any references, flush buffers
  if (m_bufferPool && m_bufferPool->HasRefs())
  {
    avcodec_flush_buffers(m_avctx);
  }
  return IHardwareDecoder::Release();
}

void CDecoder::Close()
{
  CSingleLock lock(m_section);
  m_decoder = nullptr;
  m_vcontext = nullptr;
  if (m_videoBuffer)
  {
    m_videoBuffer->Release();
    m_videoBuffer = nullptr;
  }
  memset(&m_format, 0, sizeof(m_format));

  if (m_dxva_context)
  {
    CLog::LogF(LOGNOTICE, "closing decoder.");
    m_dxva_context->Release(this);
  }
  m_dxva_context = nullptr;
}

static bool CheckH264L41(AVCodecContext *avctx)
{
    unsigned widthmbs  = (avctx->coded_width + 15) / 16;  // width in macroblocks
    unsigned heightmbs = (avctx->coded_height + 15) / 16; // height in macroblocks
    unsigned maxdpbmbs = 32768;                     // Decoded Picture Buffer (DPB) capacity in macroblocks for L4.1

    return (avctx->refs * widthmbs * heightmbs <= maxdpbmbs);
}

static bool IsL41LimitedATI()
{
  DXGI_ADAPTER_DESC AIdentifier = { 0 };
  DX::DeviceResources::Get()->GetAdapterDesc(&AIdentifier);

  if(AIdentifier.VendorId == PCIV_ATI)
  {
    for (unsigned idx = 0; UVDDeviceID[idx] != 0; idx++)
    {
      if (UVDDeviceID[idx] == AIdentifier.DeviceId)
        return true;
    }
  }
  return false;
}

static bool HasVP3WidthBug(AVCodecContext *avctx)
{
  // Some nVidia VP3 hardware cannot do certain macroblock widths
  DXGI_ADAPTER_DESC AIdentifier = { 0 };
  DX::DeviceResources::Get()->GetAdapterDesc(&AIdentifier);

  if(AIdentifier.VendorId == PCIV_nVidia
  && !CDVDCodecUtils::IsVP3CompatibleWidth(avctx->coded_width))
  {
    // Find the card in a known list of problematic VP3 hardware
    for (unsigned idx = 0; VP3DeviceID[idx] != 0; idx++)
      if (VP3DeviceID[idx] == AIdentifier.DeviceId)
        return true;
  }
  return false;
}

static bool HasATIMP2Bug(AVCodecContext *avctx)
{
  DXGI_ADAPTER_DESC AIdentifier = { 0 };
  DX::DeviceResources::Get()->GetAdapterDesc(&AIdentifier);
  if (AIdentifier.VendorId != PCIV_ATI)
    return false;

  // AMD/ATI card doesn't like some SD MPEG2 content
  // here are params of these videos
  return avctx->height <= 576
      && avctx->colorspace == AVCOL_SPC_BT470BG
      && avctx->color_primaries == AVCOL_PRI_BT470BG 
      && avctx->color_trc == AVCOL_TRC_GAMMA28;
}

// UHD HEVC Main10 causes crash on Xbox One S/X
static bool HasXbox4kHevcMain10Bug(AVCodecContext *avctx)
{
  if (CSysInfo::GetWindowsDeviceFamily() != CSysInfo::Xbox)
    return false;

  if (avctx->codec_id != AV_CODEC_ID_HEVC)
    return false;

  if (avctx->profile != FF_PROFILE_HEVC_MAIN_10)
    return false;

  if (avctx->height <= 1080 || avctx->width <= 1920)
    return false;

  return true;
}

static bool CheckCompatibility(AVCodecContext *avctx)
{
  if (HasXbox4kHevcMain10Bug(avctx))
  {
    CLog::LogFunction(LOGWARNING, "DXVA", "UHD hevc main10 is not supported on Xbox One S/X. DXVA will not be used.");
    return false;
  }

  if (avctx->codec_id == AV_CODEC_ID_MPEG2VIDEO && HasATIMP2Bug(avctx))
    return false;

  // The incompatibilities are all for H264
  if(avctx->codec_id != AV_CODEC_ID_H264)
    return true;

  // Macroblock width incompatibility
  if (HasVP3WidthBug(avctx))
  {
    CLog::LogFunction(LOGWARNING,"DXVA", "width %i is not supported with nVidia VP3 hardware. DXVA will not be used.", avctx->coded_width);
    return false;
  }

  // Check for hardware limited to H264 L4.1 (ie Bluray).

  // No advanced settings: autodetect.
  // The advanced setting lets the user override the autodetection (in case of false positive or negative)

  bool checkcompat;
  if (!g_advancedSettings.m_DXVACheckCompatibilityPresent)
    checkcompat = IsL41LimitedATI();  // ATI UVD and UVD+ cards can only do L4.1 - corresponds roughly to series 3xxx
  else
    checkcompat = g_advancedSettings.m_DXVACheckCompatibility;

  if (checkcompat && !CheckH264L41(avctx))
  {
      CLog::LogFunction(LOGWARNING, "DXVA", "compatibility check: video exceeds L4.1. DXVA will not be used.");
      return false;
  }

  return true;
}

bool CDecoder::Open(AVCodecContext *avctx, AVCodecContext* mainctx, enum AVPixelFormat fmt)
{
  if (!CheckCompatibility(avctx))
    return false;

  CSingleLock lock(m_section);
  Close();

  if(m_state == DXVA_LOST)
  {
    CLog::LogFunction(LOGDEBUG, "DXVA", "device is in lost state, we can't start.");
    return false;
  }

  CLog::LogFunction(LOGDEBUG, "DXVA", "open decoder.");
  if (!CDXVAContext::EnsureContext(&m_dxva_context, this))
    return false;

  if (!m_dxva_context->GetFormatAndConfig(avctx, m_format, *m_context->cfg))
  {
    CLog::LogFunction(LOGDEBUG, "DXVA", "unable to find an input/output format combination.");
    return false;
  }

  CLog::LogFunction(LOGDEBUG, "DXVA", "selected output format: %d.", m_format.OutputFormat);
  CLog::LogFunction(LOGDEBUG, "DXVA", "source requires %d references.", avctx->refs);
  if (m_format.Guid == DXVADDI_Intel_ModeH264_E && avctx->refs > 11)
  {
    const dxva2_mode_t *mode = dxva2_find_mode(&m_format.Guid);
    CLog::LogFunction(LOGWARNING, "DXVA", "too many references %d for selected decoder '%s'.", avctx->refs, mode->name);
    return false;
  }

  if (7 > m_shared)
    m_shared = 7;

  m_refs = 2; // 1 decode + 1 safety
  m_surface_alignment = 16;

  switch (avctx->codec_id)
  {
  case AV_CODEC_ID_MPEG2VIDEO:
    /* decoding MPEG-2 requires additional alignment on some Intel GPUs,
    but it causes issues for H.264 on certain AMD GPUs..... */
    m_surface_alignment = 32;
    m_refs += 2 + 2;
    break;
  case AV_CODEC_ID_HEVC:
    /* the HEVC DXVA2 spec asks for 128 pixel aligned surfaces to ensure
    all coding features have enough room to work with */
    m_surface_alignment = 128;
    m_refs += 16;
    break;
  case AV_CODEC_ID_H264:
    m_refs += 16;
    break;
  case AV_CODEC_ID_VP9:
    m_refs += 4;
    break;
  default:
    m_refs += 2;
  }

  if (avctx->active_thread_type & FF_THREAD_FRAME)
    m_refs += avctx->thread_count;

  m_bufferPool = std::make_shared<CDXVABufferPool>();

  if(!OpenDecoder())
    return false;

  avctx->get_buffer2 = CDecoder::FFGetBuffer;
  avctx->hwaccel_context = m_context;
  avctx->slice_flags = SLICE_FLAG_ALLOW_FIELD | SLICE_FLAG_CODED_ORDER;

  mainctx->get_buffer2 = CDecoder::FFGetBuffer;
  mainctx->hwaccel_context = m_context;
  mainctx->slice_flags = SLICE_FLAG_ALLOW_FIELD | SLICE_FLAG_CODED_ORDER;

  m_avctx = mainctx;
  DXGI_ADAPTER_DESC AIdentifier = { 0 };
  DX::DeviceResources::Get()->GetAdapterDesc(&AIdentifier);
  if (AIdentifier.VendorId == PCIV_Intel && m_format.Guid == DXVADDI_Intel_ModeH264_E)
  {
#ifdef FF_DXVA2_WORKAROUND_INTEL_CLEARVIDEO
    m_context->workaround |= FF_DXVA2_WORKAROUND_INTEL_CLEARVIDEO;
#else
    CLog::LogFunction(LOGWARNING, "DXVA", "used Intel ClearVideo decoder, but no support workaround for it in libavcodec.");
#endif
  }
  else if (AIdentifier.VendorId == PCIV_ATI && IsL41LimitedATI())
  {
#ifdef FF_DXVA2_WORKAROUND_SCALING_LIST_ZIGZAG
    m_context->workaround |= FF_DXVA2_WORKAROUND_SCALING_LIST_ZIGZAG;
#else
    CLog::LogFunction(LOGWARNING, "DXVA", "video card with different scaling list zigzag order detected, but no support in libavcodec.");
#endif
  }

  std::list<EINTERLACEMETHOD> deintMethods;
  deintMethods.push_back(VS_INTERLACEMETHOD_NONE);
  deintMethods.push_back(VS_INTERLACEMETHOD_DXVA_AUTO);
  m_processInfo.UpdateDeinterlacingMethods(deintMethods);
  m_processInfo.SetDeinterlacingMethodDefault(VS_INTERLACEMETHOD_DXVA_AUTO);

  m_state = DXVA_OPEN;
  return true;
}

CDVDVideoCodec::VCReturn CDecoder::Decode(AVCodecContext* avctx, AVFrame* frame)
{
  CSingleLock lock(m_section);
  CDVDVideoCodec::VCReturn result = Check(avctx);
  if(result != CDVDVideoCodec::VC_NONE)
    return result;

  if(frame)
  {
    if (m_bufferPool->IsValid(reinterpret_cast<ID3D11View*>(frame->data[3])))
    {
      SAFE_RELEASE(m_videoBuffer);
      m_videoBuffer = reinterpret_cast<CDXVAOutputBuffer*>(m_bufferPool->Get());
      if (!m_videoBuffer)
      {
        CLog::LogFunction(ERROR, "DXVA", "ran out of buffers.");
        return CDVDVideoCodec::VC_ERROR;
      }
      m_videoBuffer->SetRef(frame);
      m_videoBuffer->format = m_format.OutputFormat;
      m_videoBuffer->width = FFALIGN(m_format.SampleWidth, m_surface_alignment);
      m_videoBuffer->height = FFALIGN(m_format.SampleHeight, m_surface_alignment);
      return CDVDVideoCodec::VC_PICTURE;
    }
    CLog::LogFunction(LOGWARNING, "DXVA", "ignoring invalid surface.");
    return CDVDVideoCodec::VC_BUFFER;
  }

  return CDVDVideoCodec::VC_BUFFER;
}

bool CDecoder::GetPicture(AVCodecContext* avctx, VideoPicture* picture)
{
  SAFE_RELEASE(picture->videoBuffer);

  static_cast<ICallbackHWAccel*>(avctx->opaque)->GetPictureCommon(picture);
  CSingleLock lock(m_section);

  picture->videoBuffer = m_videoBuffer;
  m_videoBuffer = nullptr;

  int queued, discard, free;
  m_processInfo.GetRenderBuffers(queued, discard, free);
  if (free > 1)
  {
    DX::Windowing().RequestDecodingTime();
  }
  else
  {
    DX::Windowing().ReleaseDecodingTime();
  }

  return true;
}

void CDecoder::Reset()
{
  SAFE_RELEASE(m_videoBuffer);
}

CDVDVideoCodec::VCReturn CDecoder::Check(AVCodecContext* avctx)
{
  CSingleLock lock(m_section);

  // we may not have a hw decoder on systems (AMD HD2xxx, HD3xxx) which are only capable
  // of opening a single decoder and VideoPlayer opened a new stream without having flushed
  // current one.
  if (!m_decoder)
    return CDVDVideoCodec::VC_BUFFER;

  if(m_state == DXVA_RESET)
    Close();

  if(m_state == DXVA_LOST)
  {
    Close();
    lock.Leave();
    m_event.WaitMSec(2000);
    lock.Enter();
    if(m_state == DXVA_LOST)
    {
      CLog::LogF(LOGERROR, "device didn't reset in reasonable time.");
      return CDVDVideoCodec::VC_ERROR;
    }
  }

  if(m_format.SampleWidth  == 0
  || m_format.SampleHeight == 0)
  {
    if(!Open(avctx, avctx, avctx->pix_fmt))
    {
      CLog::LogF(LOGERROR, "decoder was not able to reset.");
      Close();
      return CDVDVideoCodec::VC_ERROR;
    }
    return CDVDVideoCodec::VC_FLUSHED;
  }
  else
  {
    if(avctx->refs > m_refs)
    {
      CLog::LogF(LOGWARNING, "number of required reference frames increased, recreating decoder.");
      Close();
      return CDVDVideoCodec::VC_FLUSHED;
    }
  }

  // Status reports are available only for the DXVA2_ModeH264 and DXVA2_ModeVC1 modes
  if(avctx->codec_id != AV_CODEC_ID_H264
  && avctx->codec_id != AV_CODEC_ID_VC1
  && avctx->codec_id != AV_CODEC_ID_WMV3)
    return CDVDVideoCodec::VC_NONE;
  
#ifdef TARGET_WINDOWS_DESKTOP
  D3D11_VIDEO_DECODER_EXTENSION data = {0};
  union {
    DXVA_Status_H264 h264;
    DXVA_Status_VC1  vc1;
  } status = {};

  /* I'm not sure, but MSDN says nothing about extensions functions in D3D11, try to using with same way as in DX9 */
  data.Function = DXVA_STATUS_REPORTING_FUNCTION;
  data.pPrivateOutputData    = &status;
  data.PrivateOutputDataSize = avctx->codec_id == AV_CODEC_ID_H264 ? sizeof(DXVA_Status_H264) : sizeof(DXVA_Status_VC1);
  HRESULT hr;
  if (FAILED(hr = m_dxva_context->GetVideoContext()->DecoderExtension(m_decoder.Get(), &data)))
  {
    CLog::LogFunction(LOGWARNING, "DXVA", "failed to get decoder status - 0x%08X.", hr);
    return CDVDVideoCodec::VC_ERROR;
  }

  if(avctx->codec_id == AV_CODEC_ID_H264)
  {
    if(status.h264.bStatus)
      CLog::LogFunction(LOGWARNING, "DXVA", "decoder problem of status %d with %d.", status.h264.bStatus, status.h264.bBufType);
  }
  else
  {
    if(status.vc1.bStatus)
      CLog::LogFunction(LOGWARNING, "DXVA", "decoder problem of status %d with %d.", status.vc1.bStatus, status.vc1.bBufType);
  }
#endif
  return CDVDVideoCodec::VC_NONE;
}

bool CDecoder::OpenDecoder()
{
  m_decoder = nullptr;
  m_vcontext = nullptr;
  m_context->decoder = nullptr;
  m_context->video_context = nullptr;

  m_context->surface_count = m_refs + m_shared; // refs + processor buffer

  if (!m_dxva_context->CreateSurfaces(m_format, m_context->surface_count, m_surface_alignment, m_context->surface))
    return false;

  for (unsigned i = 0; i < m_context->surface_count; i++)
    m_bufferPool->AddView(m_context->surface[i]);

  if (!m_dxva_context->CreateDecoder(m_format, *m_context->cfg, m_decoder.GetAddressOf(), m_vcontext.GetAddressOf()))
    return false;

  m_context->decoder = m_decoder.Get();
  m_context->video_context = m_vcontext.Get();

  return true;
}

bool CDecoder::Supports(enum AVPixelFormat fmt)
{
  if(fmt == AV_PIX_FMT_D3D11VA_VLD)
    return true;
  return false;
}

void CDecoder::FFReleaseBuffer(void* opaque, uint8_t* data)
{
  CDecoder* decoder = static_cast<CDecoder*>(opaque);
  decoder->ReleaseBuffer(data);
}

void CDecoder::ReleaseBuffer(uint8_t *data)
{
  ID3D11VideoDecoderOutputView* view = reinterpret_cast<ID3D11VideoDecoderOutputView*>(data);
  if (!m_bufferPool->IsValid(view))
  {
    CLog::LogF(LOGWARNING, "return of invalid surface.");
  }
  m_bufferPool->ReturnView(view);

  IHardwareDecoder::Release();
}

int CDecoder::FFGetBuffer(AVCodecContext* avctx, AVFrame* pic, int flags)
{
  ICallbackHWAccel* cb = static_cast<ICallbackHWAccel*>(avctx->opaque);
  CDecoder* decoder = static_cast<CDecoder*>(cb->GetHWAccel());

  return decoder->GetBuffer(avctx, pic);
}

int CDecoder::GetBuffer(AVCodecContext *avctx, AVFrame *pic)
{
  if (!m_decoder)
    return -1;

  ID3D11View* view = m_bufferPool->GetView();
  if (view == nullptr)
  {
    CLog::LogF(LOGERROR, "no surface available.");
    m_state = DXVA_LOST;
    return -1;
  }

  pic->reordered_opaque = avctx->reordered_opaque;

  for(unsigned i = 0; i < 4; i++)
  {
    pic->data[i] = nullptr;
    pic->linesize[i] = 0;
  }

  pic->data[0] = reinterpret_cast<uint8_t*>(view);
  pic->data[3] = reinterpret_cast<uint8_t*>(view);
  AVBufferRef *buffer = av_buffer_create(pic->data[3], 0, CDecoder::FFReleaseBuffer, this, 0);
  if (!buffer)
  {
    CLog::LogF(LOGERROR, "error creating buffer.");
    return -1;
  }
  pic->buf[0] = buffer;

  Acquire();

  return 0;
}

unsigned CDecoder::GetAllowedReferences()
{
  return m_shared;
}

void CDecoder::CloseDXVADecoder()
{
  CSingleLock lock(m_section);
  m_decoder = nullptr;
}
