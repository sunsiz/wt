/*
 * Copyright (C) 2017 Emweb bvba, Herent, Belgium.
 *
 * See the LICENSE file for terms of use.
 */

#include "Wt/WRasterImage"

#include "Wt/WBrush"
#include "Wt/WException"
#include "Wt/WFontMetrics"
#include "Wt/WGradient"
#include "Wt/WLogger"
#include "Wt/WPainter"
#include "Wt/WPen"
#include "Wt/WString"
#include "Wt/WTransform"
#include "Wt/Http/Response"

#include "UriUtils.h"

#include <cmath>

#include <boost/algorithm/string.hpp>
#include <boost/lexical_cast.hpp>

// Direct2D
#include <d2d1.h>
#include <d2d1helper.h>
// WIC
#include <wincodec.h>
// DirectWrite
#include <Dwrite.h>

#include <Shlwapi.h> // for SHCreateMemStream

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// #define DEBUG_D2D

namespace {

template <class T> void SafeRelease(T *&ppT)
{
  if (ppT) {
    ppT->Release();
    ppT = NULL;
  }
}

D2D1_COLOR_F fromWColor(const Wt::WColor &color)
{
  return D2D1::ColorF(color.red()/255.0f, color.green()/255.0f, color.blue()/255.0f, color.alpha()/255.0f);
}

D2D1_POINT_2F fromPointF(const Wt::WPointF &point)
{
  return D2D1::Point2F(static_cast<FLOAT>(point.x()), static_cast<FLOAT>(point.y()));
}

bool fequal(double d1, double d2)
{
  return std::fabs(d1 - d2) < 1E-5;
}

enum DrawTag1 {
  NO_DRAW_TAG = 0,
  DRAW_LINE = 1,
  DRAW_PATH = 2,
  DRAW_CLIP_PATH = 3,
  CLEAR_TAG = 4,
  PUSH_LAYER = 5,
  POP_LAYER = 6,
  DRAW_BITMAP = 7,
  SET_ANTIALIAS_MODE = 8
};

enum DrawTag2 {
  APPLY_TRANSFORM = 1,
  SET_TRANSFORM = 2,
  DRAW_PLAIN_PATH = 3
};
}

// FIXME: word wrap
// FIXME: make FontSupportDirectWrite and use it

namespace Wt {

LOGGER("WRasterImage");

class WRasterImage::Impl {
public:
  Impl():
    w_(0),
    h_(0),
    drawingCount_(0),
    factory_(NULL),
    rt_(NULL),
    wicFactory_(NULL),
    bitmap_(NULL),
    fillBrush_(NULL),
    fillBrushStyle_(SolidPattern),
    strokeBrush_(NULL),
    stroke_(NULL),
    lineWidth_(0.f),
    clipGeometry_(NULL),
    clipLayer_(NULL),
    clipLayerActive_(false),
    writeFactory_(NULL),
    textFormat_(NULL),
    font_(NULL)
  {}

  ~Impl()
  {
    SafeRelease(fillBrush_);
    SafeRelease(strokeBrush_);
    SafeRelease(stroke_);
    SafeRelease(clipGeometry_);
    SafeRelease(clipLayer_);
    SafeRelease(bitmap_);
    SafeRelease(wicFactory_);
    SafeRelease(rt_);
    SafeRelease(factory_);

    SafeRelease(textFormat_);
    SafeRelease(font_);
    SafeRelease(writeFactory_);
  }

  unsigned w_, h_;
  int drawingCount_;
  std::string type_;

  ID2D1Factory *factory_;
  ID2D1RenderTarget *rt_;
  IWICImagingFactory* wicFactory_;
  IWICBitmap* bitmap_;
  ID2D1Brush *fillBrush_;
  BrushStyle fillBrushStyle_;
  ID2D1SolidColorBrush *strokeBrush_;
  ID2D1StrokeStyle *stroke_;
  FLOAT lineWidth_;
  ID2D1PathGeometry *clipGeometry_;
  ID2D1Layer *clipLayer_;
  bool clipLayerActive_;

  IDWriteFactory *writeFactory_;
  IDWriteTextFormat *textFormat_;
  IDWriteFont *font_;

  void applyTransform(const WTransform& t);
  void setTransform(const WTransform& t);
  void drawPlainPath(ID2D1PathGeometry *p, const WPainterPath& path, bool filled);

  void beginDraw()
  {
    ++drawingCount_;
    if (drawingCount_ == 1)
      rt_->BeginDraw();
  }

  void endDraw()
  {
    --drawingCount_;
    if (drawingCount_ == 0) {
      D2D1_TAG tag1, tag2;
      HRESULT hr = rt_->EndDraw(&tag1, &tag2);
      if (!SUCCEEDED(hr)) {
#ifdef DEBUG_D2D
        LOG_ERROR("D2D error during drawing: HRESULT " << hr << ", active tags: tag1: " << tag1 << ", tag2: " << tag2);
#else
        LOG_ERROR("D2D error during drawing: HRESULT " << hr);
#endif
      }
    }
  }

  void resumeDraw()
  {
    if (drawingCount_ == 0)
      return;

    rt_->BeginDraw();
    if (clipLayerActive_)
      rt_->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clipGeometry_), clipLayer_);
  }

  void suspendDraw()
  {
    if (drawingCount_ == 0)
      return;

    if (clipLayerActive_)
      rt_->PopLayer();
    D2D1_TAG tag1, tag2;
    HRESULT hr = rt_->EndDraw(&tag1, &tag2);
    if (!SUCCEEDED(hr)) {
#ifdef DEBUG_D2D
      LOG_ERROR("D2D error during drawing: HRESULT " << hr << ", active tags: tag1: " << tag1 << ", tag2: " << tag2);
#else
      LOG_ERROR("D2D error during drawing: HRESULT " << hr);
#endif
    }
  }

  class DrawTagGuard {
  public:
    DrawTagGuard(WRasterImage::Impl *impl, int tagId, D2D1_TAG val)
      : impl_(impl),
        tagId_(tagId)
    {
      D2D1_TAG tag1, tag2;
      impl_->rt_->GetTags(&tag1, &tag2);
      if (tagId == 1) {
        impl_->rt_->SetTags(val, tag2);
	oldTag_ = tag1;
      } else if (tagId == 2) {
	impl_->rt_->SetTags(tag2, val);
	oldTag_ = tag2;
      }
    }

    ~DrawTagGuard()
    {
      D2D1_TAG tag1, tag2;
      impl_->rt_->GetTags(&tag1, &tag2);
      if (tagId_ == 1) {
        impl_->rt_->SetTags(oldTag_, tag2);
      } else {
	impl_->rt_->SetTags(tag1, oldTag_);
      }
    }

  private:
    WRasterImage::Impl *impl_;
    int tagId_;
    D2D1_TAG oldTag_;
  };
};

#ifdef DEBUG_D2D
#define GUARD_TAG(tag) Impl::DrawTagGuard _tag_guard(impl_, 1, tag)
#define GUARD_TAG_IMPL(tag) Impl::DrawTagGuard _tag_guard_impl(this, 2, tag)
#else // DEBUG_D2D
#define GUARD_TAG(...)
#define GUARD_TAG_IMPL(...)
#endif // DEBUG_D2D

WRasterImage::WRasterImage(const std::string& type,
			   const WLength& width, const WLength& height,
			   WObject *parent)
  : WResource(parent),
    width_(width),
    height_(height),
    painter_(0),
    impl_(new Impl)
{
  impl_->type_ = type;
  impl_->w_ = static_cast<unsigned long>(width.toPixels());
  impl_->h_ = static_cast<unsigned long>(height.toPixels());
  
  if (!impl_->w_ || !impl_->h_) {
    impl_->bitmap_ = 0;
    return;
  }

  HRESULT hr = S_OK;

  WICPixelFormatGUID format = GUID_WICPixelFormat32bppPRGBA;

  if (SUCCEEDED(hr)) {
    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);

    if (!SUCCEEDED(hr)) {
      delete impl_;
      throw WException("D2D: Error initializing COM: HRESULT " + boost::lexical_cast<std::string>(hr));
    }
  }

  // Create a Direct2D factory.
  if (SUCCEEDED(hr))
    hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &impl_->factory_);

  if (SUCCEEDED(hr))
    hr = CoCreateInstance(CLSID_WICImagingFactory, 0, CLSCTX_INPROC_SERVER,
			  IID_IWICImagingFactory, (void**)&impl_->wicFactory_);

  if (SUCCEEDED(hr))
    hr = impl_->wicFactory_->CreateBitmap(impl_->w_, impl_->h_,
					  format,
					  WICBitmapCacheOnLoad, &impl_->bitmap_);

  struct D2D1_RENDER_TARGET_PROPERTIES rtp;
  rtp.type = D2D1_RENDER_TARGET_TYPE_DEFAULT; // HW if available, else SW (not for WIC, so always SW)
  rtp.pixelFormat = D2D1::PixelFormat(DXGI_FORMAT_R8G8B8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
  rtp.dpiX = 0;
  rtp.dpiY = 0;
  rtp.usage = D2D1_RENDER_TARGET_USAGE_NONE;
  rtp.minLevel = D2D1_FEATURE_LEVEL_DEFAULT;

  if (SUCCEEDED(hr))
    hr = impl_->factory_->CreateWicBitmapRenderTarget(impl_->bitmap_, rtp, &impl_->rt_);

  if (SUCCEEDED(hr))
    hr = impl_->rt_->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0),
                     reinterpret_cast<ID2D1SolidColorBrush**>(&impl_->fillBrush_));

  if (SUCCEEDED(hr))
    hr = impl_->rt_->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0),
					   &impl_->strokeBrush_);

  if (SUCCEEDED(hr))
    hr = impl_->factory_->CreateStrokeStyle(
      D2D1::StrokeStyleProperties(),
      0,
      0,
      &impl_->stroke_
    );

  if (SUCCEEDED(hr))
    hr = impl_->factory_->CreatePathGeometry(&impl_->clipGeometry_);

  if (SUCCEEDED(hr))
    hr = impl_->rt_->CreateLayer(&impl_->clipLayer_);
  
  // not clear from documentation: do we have to share the factory created
  // by this call in order to benefit from shared font caching etc, or will
  // a shared font cache be used by specifying the SHARED option? When
  // specifying the SHARED option, is the factory thread-safe? We take the
  // safe approach, and assume that each specifying SHARED will cause state
  // to be shared in a thread-safe manner
  if (SUCCEEDED(hr))
    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
      __uuidof(impl_->writeFactory_),
      reinterpret_cast<IUnknown **>(&impl_->writeFactory_)
    );

  if (SUCCEEDED(hr))
    hr = impl_->writeFactory_->CreateTextFormat(L"Times New Roman", 0,
					        DWRITE_FONT_WEIGHT_NORMAL,
					        DWRITE_FONT_STYLE_NORMAL,
					        DWRITE_FONT_STRETCH_NORMAL,
					        12.0,
					        L"",
					        &impl_->textFormat_);

  IDWriteFontCollection *sysFontCollection = NULL;
  if (SUCCEEDED(hr))
    hr = impl_->textFormat_->GetFontCollection(&sysFontCollection);
  UINT32 fontIndex = UINT32_MAX;
  BOOL fontExists = FALSE;
  if (SUCCEEDED(hr))
    hr = sysFontCollection->FindFamilyName(L"Times New Roman", &fontIndex, &fontExists);
  if (SUCCEEDED(hr) && !fontExists) {
    // Should not happen, something's horribly wrong
    throw WException("Could not locate default Times New Roman font in system font collection");
  }
  IDWriteFontFamily *fontFamily = NULL;
  if (SUCCEEDED(hr))
    hr = sysFontCollection->GetFontFamily(fontIndex, &fontFamily);
  if (SUCCEEDED(hr))
    hr = fontFamily->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, &impl_->font_);

  SafeRelease(fontFamily);
  SafeRelease(sysFontCollection);

  if (!SUCCEEDED(hr)) {
    delete impl_;
    throw WException(std::string("Error when initializing D2D: HRESULT ") + boost::lexical_cast<std::string>(hr));
  }
}
  
void WRasterImage::clear()
{
  impl_->beginDraw();
  GUARD_TAG(CLEAR_TAG);
  impl_->rt_->Clear();
  impl_->endDraw();
}

WRasterImage::~WRasterImage()
{
  beingDeleted();

  delete impl_;

  CoUninitialize();
}

void WRasterImage::addFontCollection(const std::string& directory,
				     bool recursive)
{
#if 0
  fontSupport_->addFontCollection(directory, recursive);
#endif
}
  
WFlags<WPaintDevice::FeatureFlag> WRasterImage::features() const
{
  return HasFontMetrics;
}

void WRasterImage::init()
{
  if (!impl_->w_ || !impl_->h_)
    throw WException("Raster image should have non-0 width and height");

  // save unit matrix & empty clipping
  //impl_->canvas_->save();

  impl_->beginDraw();

  setChanged(Clipping | Transform | Pen | Brush | Font | Hints);
}

void WRasterImage::done()
{
  if (impl_->clipLayerActive_) {
    impl_->rt_->PopLayer();
  }

  impl_->endDraw();
}

void WRasterImage::Impl::applyTransform(const WTransform& t)
{
  GUARD_TAG_IMPL(APPLY_TRANSFORM);
  D2D1_MATRIX_3X2_F currentMatrix;
  rt_->GetTransform(&currentMatrix);
  D2D1::Matrix3x2F matrix(
    static_cast<FLOAT>(t.m11()),
    static_cast<FLOAT>(t.m12()),
    static_cast<FLOAT>(t.m21()),
    static_cast<FLOAT>(t.m22()),
    static_cast<FLOAT>(t.dx()),
    static_cast<FLOAT>(t.dy()));
  rt_->SetTransform(currentMatrix * matrix);
}

void WRasterImage::Impl::setTransform(const WTransform& t)
{
  GUARD_TAG_IMPL(SET_TRANSFORM);
  D2D1::Matrix3x2F matrix(
    static_cast<FLOAT>(t.m11()),
    static_cast<FLOAT>(t.m12()),
    static_cast<FLOAT>(t.m21()),
    static_cast<FLOAT>(t.m22()),
    static_cast<FLOAT>(t.dx()),
    static_cast<FLOAT>(t.dy()));
  rt_->SetTransform(matrix);
}

void WRasterImage::setChanged(WFlags<ChangeFlag> flags)
{
  HRESULT hr = S_OK;
  if (flags & Clipping) {
    GUARD_TAG(DRAW_CLIP_PATH);
    if (impl_->clipLayerActive_) {
      GUARD_TAG(POP_LAYER);
      impl_->rt_->PopLayer();
      impl_->clipLayerActive_ = false;
    }
    if (painter()->hasClipping()) {
      impl_->setTransform(painter()->clipPathTransform());
      SafeRelease(impl_->clipGeometry_);
      if (SUCCEEDED(hr))
        hr = impl_->factory_->CreatePathGeometry(&impl_->clipGeometry_);
      if (SUCCEEDED(hr))
        impl_->drawPlainPath(impl_->clipGeometry_, painter()->clipPath(), true);
      SafeRelease(impl_->clipLayer_);
      if (SUCCEEDED(hr))
        hr = impl_->rt_->CreateLayer(&impl_->clipLayer_);
      if (SUCCEEDED(hr)) {
        GUARD_TAG(PUSH_LAYER);
        impl_->rt_->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), impl_->clipGeometry_), impl_->clipLayer_);
      }
      impl_->clipLayerActive_ = true;
      impl_->setTransform(painter()->combinedTransform());
    }
    if (!SUCCEEDED(hr)) {
      LOG_ERROR("D2D error when creating clip path: HRESULT " << hr);
    }
  }

  if (flags & Transform) {
    impl_->setTransform(painter()->combinedTransform());
    flags = Pen | Brush | Font | Hints;
  }

  if (flags & Hints) {
    GUARD_TAG(SET_ANTIALIAS_MODE);
    if (!(painter()->renderHints() & WPainter::Antialiasing)) {
      impl_->rt_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
    } else {
      impl_->rt_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    }
  }

  if (flags & Pen) {
    const WPen& pen = painter()->pen();

    if (pen.style() != NoPen) {
      const WColor& color = pen.color();

      D2D1_STROKE_STYLE_PROPERTIES strokeProperties = D2D1::StrokeStyleProperties();
      impl_->strokeBrush_->SetColor(fromWColor(color));

      WLength w = pen.width();
      impl_->lineWidth_ = static_cast<FLOAT>(painter()->normalizedPenWidth(w, w.value() == 0).toPixels());

      switch (pen.capStyle()) {
      case FlatCap:
	strokeProperties.startCap = strokeProperties.endCap = strokeProperties.dashCap = D2D1_CAP_STYLE_FLAT;
	break;
      case SquareCap:
	strokeProperties.startCap = strokeProperties.endCap = strokeProperties.dashCap = D2D1_CAP_STYLE_SQUARE;
	break;
      case RoundCap:
	strokeProperties.startCap = strokeProperties.endCap = strokeProperties.dashCap = D2D1_CAP_STYLE_ROUND;
	break;
      }

      switch (pen.joinStyle()) {
      case MiterJoin:
	strokeProperties.lineJoin = D2D1_LINE_JOIN_MITER;
	break;
      case BevelJoin:
	strokeProperties.lineJoin = D2D1_LINE_JOIN_BEVEL;
	break;
      case RoundJoin:
	strokeProperties.lineJoin = D2D1_LINE_JOIN_ROUND;
	break;
      }

      float dashes[20];
      int numdashes = 0;

      switch (pen.style()) {
      case NoPen:
	break;
      case SolidLine:
	break;
      case DashLine: {
	const float dasharray[] = {4, 2};
	memcpy(dashes, dasharray, sizeof(dasharray));
	numdashes = sizeof(dasharray) / sizeof(dasharray[0]);
	break;
      }
      case DotLine: {
	const float dasharray[] = {1, 2};
	memcpy(dashes, dasharray, sizeof(dasharray));
	numdashes = sizeof(dasharray) / sizeof(dasharray[0]);
	break;
      }
      case DashDotLine: {
	const float dasharray[] = {4, 2, 1, 2};
	memcpy(dashes, dasharray, sizeof(dasharray));
	numdashes = sizeof(dasharray) / sizeof(dasharray[0]);
	break;
      }
      case DashDotDotLine: {
	const float dasharray[] = {4, 2, 1, 2, 1, 2};
	memcpy(dashes, dasharray, sizeof(dasharray));
	numdashes = sizeof(dasharray) / sizeof(dasharray[0]);
	break;
      }
      }
      
      SafeRelease(impl_->stroke_);

      hr = impl_->factory_->CreateStrokeStyle(
	strokeProperties,
	numdashes > 0 ? dashes : NULL, numdashes,
	&impl_->stroke_
      );

    }
  }

  if (flags & Brush) {
    const WBrush& brush = painter()->brush();
    if (brush.style() == SolidPattern) {
      const WColor &color = painter()->brush().color();
      if (impl_->fillBrushStyle_ != SolidPattern) {
        SafeRelease(impl_->fillBrush_);
        hr = impl_->rt_->CreateSolidColorBrush(fromWColor(color),
          reinterpret_cast<ID2D1SolidColorBrush**>(&impl_->fillBrush_));
        impl_->fillBrushStyle_ = SolidPattern;
      } else {
        reinterpret_cast<ID2D1SolidColorBrush*>(impl_->fillBrush_)->SetColor(fromWColor(color));
      }
    } else if (brush.style() == GradientPattern) {
      const WGradient &gradient = painter()->brush().gradient();
      const std::vector<WGradient::ColorStop> &colorstops = gradient.colorstops();
      std::vector<D2D1_GRADIENT_STOP> gradientStops;
      gradientStops.reserve(colorstops.size());
      for (std::size_t i = 0; i < colorstops.size(); ++i) {
        gradientStops.push_back(D2D1::GradientStop(static_cast<FLOAT>(colorstops[i].position()), fromWColor(colorstops[i].color())));
      }
      ID2D1GradientStopCollection *gradientStopCollection = NULL;
      hr = impl_->rt_->CreateGradientStopCollection(
        &gradientStops[0],
        gradientStops.size(),
        &gradientStopCollection
      );
      SafeRelease(impl_->fillBrush_);
      impl_->fillBrushStyle_ = GradientPattern;
      if (gradient.style() == LinearGradient) {
        const WLineF &vector = gradient.linearGradientVector();
        D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES properties;
        properties.startPoint = D2D1::Point2F(static_cast<FLOAT>(vector.x1()), static_cast<FLOAT>(vector.y1()));
        properties.endPoint = D2D1::Point2F(static_cast<FLOAT>(vector.x2()), static_cast<FLOAT>(vector.y2()));
        hr = impl_->rt_->CreateLinearGradientBrush(properties, gradientStopCollection,
          reinterpret_cast<ID2D1LinearGradientBrush**>(&impl_->fillBrush_));
      } else if (gradient.style() == RadialGradient) {
        D2D1_RADIAL_GRADIENT_BRUSH_PROPERTIES properties;
        properties.center = fromPointF(gradient.radialCenterPoint());
        const WPointF &focalPoint = gradient.radialFocalPoint();
        properties.gradientOriginOffset = D2D1::Point2F(static_cast<FLOAT>(focalPoint.x()) - properties.center.x,
                                                        static_cast<FLOAT>(focalPoint.y()) - properties.center.y);
        properties.radiusX = properties.radiusY = static_cast<FLOAT>(gradient.radialRadius());
        hr = impl_->rt_->CreateRadialGradientBrush(properties, gradientStopCollection,
          reinterpret_cast<ID2D1RadialGradientBrush**>(&impl_->fillBrush_));
      }
      SafeRelease(gradientStopCollection);
    }
  }

  if (flags & Font) {
    const WFont& font = painter()->font();

    IDWriteFontCollection *sysFontCollection;
    hr = impl_->writeFactory_->GetSystemFontCollection(&sysFontCollection);

    if (!SUCCEEDED(hr))
      LOG_ERROR("DirectWrite error: failed to get system font collection");

    std::wstring fontFamilyName;
    WString families = font.specificFamilies();
    if (!families.empty()) {
      std::wstring wFamilies = families;
      std::vector<std::wstring> splitFamilies;
      boost::split(splitFamilies, wFamilies, boost::is_any_of(L","));
      for (std::size_t i = 0; i < splitFamilies.size(); ++i) {
        boost::trim_if(splitFamilies[i], boost::is_any_of(L"\"' "));
        const std::wstring &family = splitFamilies[i];
        UINT32 familyIndex = UINT32_MAX;
        BOOL found = false;
        sysFontCollection->FindFamilyName(family.c_str(), &familyIndex, &found);
        if (found) {
          fontFamilyName = family;
          break;
        }
      }
    }

    if (fontFamilyName.empty()) {
      switch (font.genericFamily()) {
      case WFont::Default:
      case WFont::Serif:
        fontFamilyName = L"Times New Roman";
        break;
      case WFont::SansSerif:
        fontFamilyName = L"Arial";
        break;
      case WFont::Monospace:
        fontFamilyName = L"Consolas";
        break;
      case WFont::Fantasy:
        fontFamilyName = L"Gabriola"; // IE/Edge on Windows choose Gabriola, so let's use that
        break;
      case WFont::Cursive:
        fontFamilyName = L"Comic Sans MS"; // Apparently, all browsers on Windows choose Comic Sans
      }
    }

    DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL;
    switch (font.weight()) {
    case WFont::Lighter:
      weight = DWRITE_FONT_WEIGHT_LIGHT;
      break;
    case WFont::NormalWeight:
      weight = DWRITE_FONT_WEIGHT_NORMAL;
      break;
    case WFont::Bold:
      weight = DWRITE_FONT_WEIGHT_BOLD;
      break;
    case WFont::Bolder:
      weight = DWRITE_FONT_WEIGHT_EXTRA_BOLD;
      break;
    default:
      weight = (DWRITE_FONT_WEIGHT)font.weightValue();
    }

    DWRITE_FONT_STYLE style = DWRITE_FONT_STYLE_NORMAL;
    switch (font.style()) {
    case WFont::NormalStyle:
      style = DWRITE_FONT_STYLE_NORMAL;
      break;
    case WFont::Oblique:
      style = DWRITE_FONT_STYLE_OBLIQUE;
      break;
    case WFont::Italic:
      style = DWRITE_FONT_STYLE_ITALIC;
      break;
    }

    SafeRelease(impl_->textFormat_);
    hr = impl_->writeFactory_->CreateTextFormat(fontFamilyName.c_str(),
                                           NULL,
					   weight,
					   style,
					   DWRITE_FONT_STRETCH_NORMAL,
					   static_cast<FLOAT>(font.sizeLength(12).toPixels()),
					   L"",
					   &impl_->textFormat_);

    SafeRelease(impl_->font_);
    UINT32 fontIndex = UINT32_MAX;
    BOOL fontExists = FALSE;
    hr = sysFontCollection->FindFamilyName(fontFamilyName.c_str(), &fontIndex, &fontExists);
    if (!fontExists) {
      throw WException("Could not locate font " + WString(fontFamilyName).toUTF8());
    }
    IDWriteFontFamily *fontFamily = NULL;
    hr = sysFontCollection->GetFontFamily(fontIndex, &fontFamily);
    hr = fontFamily->GetFirstMatchingFont(weight, DWRITE_FONT_STRETCH_NORMAL, style, &impl_->font_);

    SafeRelease(sysFontCollection);
  }
}

void WRasterImage::drawArc(const WRectF& rect,
			   double startAngle, double spanAngle)
{
  const double cx = rect.center().x();
  const double cy = rect.center().y();
  const double rx = rect.width() / 2.0;
  const double ry = rect.height() / 2.0;

  const double startAngleRad = startAngle / 180.0 * M_PI;
  const double spanAngleRad = min(spanAngle / 180.0 * M_PI, 2 * M_PI);
  const double midAngle = startAngleRad + spanAngleRad / 2.0;
  const double endAngle = startAngleRad + spanAngleRad;
  D2D1_POINT_2F startPoint = D2D1::Point2F(
    static_cast<FLOAT>(cos(-startAngleRad) * rx + cx),
    static_cast<FLOAT>(sin(-startAngleRad) * ry + cy));
  D2D1_POINT_2F midPoint = D2D1::Point2F(
    static_cast<FLOAT>(cos(-midAngle) * rx + cx),
    static_cast<FLOAT>(sin(-midAngle) * ry + cy));
  D2D1_POINT_2F endPoint = D2D1::Point2F(
    static_cast<FLOAT>(cos(-endAngle) * rx + cx),
    static_cast<FLOAT>(sin(-endAngle) * ry + cy));

  D2D1_ARC_SEGMENT arc1 = D2D1::ArcSegment(
    midPoint,
    D2D1::SizeF(static_cast<FLOAT>(rx), static_cast<FLOAT>(ry)),
    0.f,
    D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,
    D2D1_ARC_SIZE_SMALL
  );
  D2D1_ARC_SEGMENT arc2 = D2D1::ArcSegment(
    endPoint,
    D2D1::SizeF(static_cast<FLOAT>(rx), static_cast<FLOAT>(ry)),
    0.f,
    D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,
    D2D1_ARC_SIZE_SMALL
  );

  ID2D1GeometrySink *sink;
  ID2D1PathGeometry *path;
  impl_->factory_->CreatePathGeometry(&path);
  path->Open(&sink);
  sink->BeginFigure(startPoint,
		    painter()->brush().style() != NoBrush ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
  sink->AddArc(arc1);
  sink->AddArc(arc2);
  sink->EndFigure(D2D1_FIGURE_END_OPEN);
  sink->Close();

  SafeRelease(sink);

  if (painter()->brush().style() != NoBrush) {
    impl_->rt_->FillGeometry(path, impl_->fillBrush_);
  }
  if (painter()->pen().style() != NoPen) {
    impl_->rt_->DrawGeometry(path, impl_->strokeBrush_, impl_->lineWidth_, impl_->stroke_);
  }
  SafeRelease(path);
}

void WRasterImage::drawImage(const WRectF& rect, const std::string& imgUri,
			     int imgWidth, int imgHeight,
			     const WRectF& srect)
{
  ID2D1Bitmap *bitmap = NULL;
  IWICBitmapDecoder *decoder = NULL;
  IWICBitmapFrameDecode *source = NULL;
  IWICFormatConverter *converter = NULL;
  HRESULT hr = S_OK;
  if (DataUri::isDataUri(imgUri)) {
    DataUri uri(imgUri);
    IStream *istream = SHCreateMemStream(&uri.data[0], uri.data.size());
    hr = impl_->wicFactory_->CreateDecoderFromStream(
      istream,
      NULL,
      WICDecodeMetadataCacheOnLoad,
      &decoder);
    SafeRelease(istream);
    if (!SUCCEEDED(hr)) {
      throw WException("drawImage failed to read data: HRESULT " + boost::lexical_cast<std::string>(hr) + ", mime type: " + uri.mimeType);
    }
  } else {
    std::wstring wUri = WString::fromUTF8(imgUri);
    hr = impl_->wicFactory_->CreateDecoderFromFilename(
      wUri.c_str(),
      NULL,
      GENERIC_READ,
      WICDecodeMetadataCacheOnLoad,
      &decoder
    );
    if (hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND)) {
      throw WException("drawImage failed: file not found: " + imgUri);
    } else if (!SUCCEEDED(hr)) {
      throw WException("drawImage failed: HRESULT " + boost::lexical_cast<std::string>(hr) + ", uri: " + imgUri);
    }
  }
  hr = decoder->GetFrame(0, &source);
  hr = impl_->wicFactory_->CreateFormatConverter(&converter);
  hr = converter->Initialize(
    source,
    GUID_WICPixelFormat32bppPRGBA,
    WICBitmapDitherTypeNone,
    NULL,
    0.f,
    WICBitmapPaletteTypeMedianCut
  );
  hr = impl_->rt_->CreateBitmapFromWicBitmap(
    converter,
    NULL,
    &bitmap
  );

  SafeRelease(decoder);
  SafeRelease(source);
  SafeRelease(converter);

  GUARD_TAG(DRAW_BITMAP);
  impl_->rt_->DrawBitmap(
    bitmap,
    D2D1::RectF(static_cast<FLOAT>(rect.left()),
                static_cast<FLOAT>(rect.top()),
                static_cast<FLOAT>(rect.right()),
                static_cast<FLOAT>(rect.bottom())),
    1.f,
    D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,
    D2D1::RectF(static_cast<FLOAT>(srect.left()),
                static_cast<FLOAT>(srect.top()),
                static_cast<FLOAT>(srect.right()),
                static_cast<FLOAT>(srect.bottom()))
  );
  SafeRelease(bitmap);
}

void WRasterImage::drawLine(double x1, double y1, double x2, double y2)
{
  GUARD_TAG(DRAW_LINE);
  impl_->rt_->DrawLine(D2D1::Point2F(static_cast<FLOAT>(x1),
                                     static_cast<FLOAT>(y1)),
                       D2D1::Point2F(static_cast<FLOAT>(x2),
                                     static_cast<FLOAT>(y2)),
		       impl_->strokeBrush_, impl_->lineWidth_, impl_->stroke_);
}

void WRasterImage::drawPath(const WPainterPath& path)
{
  GUARD_TAG(DRAW_PATH);
  if (!path.isEmpty()) {
    ID2D1PathGeometry *p;
    impl_->factory_->CreatePathGeometry(&p);
    impl_->drawPlainPath(p, path, painter()->brush().style() != NoBrush);
    
    if (painter()->brush().style() != NoBrush) {
      impl_->rt_->FillGeometry(p, impl_->fillBrush_);
    }
    if (painter()->pen().style() != NoPen) {
      impl_->rt_->DrawGeometry(p, impl_->strokeBrush_, impl_->lineWidth_, impl_->stroke_);
    }
  }
}

void WRasterImage::setPixel(int x, int y, const WColor& c)
{
  if (painter_)
    throw WException("WRasterImage::setPixel(): cannot be used while a "
		     "painter is active");
  WICRect rect = { x, y, 1, 1 };
  IWICBitmapLock *lock = NULL;
  HRESULT hr = S_OK;
  hr = impl_->bitmap_->Lock(&rect, WICBitmapLockWrite, &lock);
  UINT bufferSize = 0;
  BYTE *buffer = NULL;

  hr = lock->GetDataPointer(&bufferSize, &buffer);
  buffer[0] = c.red();
  buffer[1] = c.green();
  buffer[2] = c.blue();
  buffer[3] = c.alpha();

  SafeRelease(lock);
}

void WRasterImage::getPixels(void *data)
{
  impl_->suspendDraw();
  HRESULT hr = impl_->bitmap_->CopyPixels(NULL, impl_->w_ * 4, impl_->w_ * impl_->h_ * 4, (BYTE *)data);
  if (!SUCCEEDED(hr)) {
    LOG_ERROR("D2D error when getting pixels: HRESULT " << hr);
  }
  impl_->resumeDraw();
}

WColor WRasterImage::getPixel(int x, int y) 
{
  impl_->suspendDraw();
  WICRect rect = { x, y, 1, 1 };
  BYTE data[4] = { 0, 0, 0, 0 };
  HRESULT hr = impl_->bitmap_->CopyPixels(&rect, impl_->w_ * 4, 4, data);
  if (!SUCCEEDED(hr)) {
    LOG_ERROR("D2D error when getting pixel " << x << "," << y << ": HRESULT " << hr);
  }
  impl_->resumeDraw();

  return WColor(data[0], data[1], data[2], data[3]);
}

void WRasterImage::Impl::drawPlainPath(ID2D1PathGeometry *p, const WPainterPath& path, bool filled)
{
  HRESULT hr = S_OK;
  GUARD_TAG_IMPL(DRAW_PLAIN_PATH);
  ID2D1GeometrySink *sink;
  hr = p->Open(&sink);

  const std::vector<WPainterPath::Segment>& segments = path.segments();

  D2D1_POINT_2F startPoint = D2D1::Point2F(0, 0);

  boolean started = false;
  for (unsigned i = 0; i < segments.size(); ++i) {
    const WPainterPath::Segment s = segments[i];

    if (s.type() != WPainterPath::Segment::MoveTo && !started) {
      sink->BeginFigure(startPoint,
			filled ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
      started = true;
    }
    switch (s.type()) {
    case WPainterPath::Segment::MoveTo:
      if (started) {
	sink->EndFigure(D2D1_FIGURE_END_OPEN);
	started = false;
      }
      startPoint = D2D1::Point2F(static_cast<FLOAT>(s.x()), static_cast<FLOAT>(s.y()));
      break;
    case WPainterPath::Segment::LineTo:
      sink->AddLine(D2D1::Point2F(static_cast<FLOAT>(s.x()), static_cast<FLOAT>(s.y())));
      break;
    case WPainterPath::Segment::CubicC1: {
      const FLOAT x1 = static_cast<FLOAT>(s.x());
      const FLOAT y1 = static_cast<FLOAT>(s.y());
      const FLOAT x2 = static_cast<FLOAT>(segments[i + 1].x());
      const FLOAT y2 = static_cast<FLOAT>(segments[i + 1].y());
      const FLOAT x3 = static_cast<FLOAT>(segments[i + 2].x());
      const FLOAT y3 = static_cast<FLOAT>(segments[i + 2].y());
      sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), D2D1::Point2F(x3, y3)));
      i += 2;
      break;
    }
    case WPainterPath::Segment::CubicC2:
    case WPainterPath::Segment::CubicEnd:
      assert(false);
      break;
    case WPainterPath::Segment::ArcC: {
      WPointF current = path.positionAtSegment(i);

      const double cx = s.x();
      const double cy = s.y();
      const double rx = segments[i+1].x();
      const double ry = segments[i+1].y();
      const double startAngle = segments[i+2].x() / 180.0 * M_PI;
      const double sweepAngle = min(segments[i+2].y() / 180.0 * M_PI, 2 * M_PI);

      const double endAngle = startAngle + sweepAngle;
      const double midAngle = startAngle + sweepAngle / 2.0;
      D2D1_POINT_2F startPoint = D2D1::Point2F(
	static_cast<FLOAT>(cos(-startAngle) * rx + cx),
	static_cast<FLOAT>(sin(-startAngle) * ry + cy));
      D2D1_POINT_2F midPoint = D2D1::Point2F(
        static_cast<FLOAT>(cos(-midAngle) * rx + cx),
	static_cast<FLOAT>(sin(-midAngle) * ry + cy));
      D2D1_POINT_2F endPoint = D2D1::Point2F(
	static_cast<FLOAT>(cos(-endAngle) * rx + cx),
	static_cast<FLOAT>(sin(-endAngle) * ry + cy));

      if (!fequal(startPoint.x, current.x()) || !fequal(startPoint.y, current.y()))
	sink->AddLine(startPoint);

      sink->AddArc(D2D1::ArcSegment(midPoint,
				    D2D1::SizeF(static_cast<FLOAT>(rx), static_cast<FLOAT>(ry)),
				    0.f,
				    sweepAngle > 0 ? D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE : D2D1_SWEEP_DIRECTION_CLOCKWISE,
				    D2D1_ARC_SIZE_SMALL));
      sink->AddArc(D2D1::ArcSegment(endPoint,
				    D2D1::SizeF(static_cast<FLOAT>(rx), static_cast<FLOAT>(ry)),
				    0.f,
				    sweepAngle > 0 ? D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE : D2D1_SWEEP_DIRECTION_CLOCKWISE,
				    D2D1_ARC_SIZE_SMALL));

      i += 2;
      break;
    }
    case WPainterPath::Segment::ArcR:
    case WPainterPath::Segment::ArcAngleSweep:
      assert(false);
      break;
    case WPainterPath::Segment::QuadC: {
      const FLOAT x1 = static_cast<FLOAT>(s.x());
      const FLOAT y1 = static_cast<FLOAT>(s.y());
      const FLOAT x2 = static_cast<FLOAT>(segments[i + 1].x());
      const FLOAT y2 = static_cast<FLOAT>(segments[i + 1].y());
      sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2)));

      i += 1;

      break;
    }
    case WPainterPath::Segment::QuadEnd:
      assert(false);
      break;
    }
  }
  if (started) {
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    started = false;
  }
  hr = sink->Close();
  SafeRelease(sink);
}

void WRasterImage::drawText(const WRectF& rect, 
			    WFlags<AlignmentFlag> flags,
			    TextFlag textFlag,
			    const WString& text,
			    const WPointF *clipPoint)
{
  D2D1_RECT_F textRect = D2D1::RectF(static_cast<FLOAT>(rect.left()),
                                     static_cast<FLOAT>(rect.top()),
                                     static_cast<FLOAT>(rect.right()),
                                     static_cast<FLOAT>(rect.bottom()));

  if (clipPoint && painter() && !painter()->clipPath().isEmpty()) {
    if (!painter()->clipPathTransform().map(painter()->clipPath())
	  .isPointInPath(painter()->worldTransform().map(*clipPoint)))
      return;
  }

  std::wstring txt = text;

  AlignmentFlag horizontalAlign = flags & AlignHorizontalMask;
  AlignmentFlag verticalAlign = flags & AlignVerticalMask;
  
  const WTransform& t = painter()->combinedTransform();
  
  WPointF p;

  switch (verticalAlign) {
  default:
  case AlignTop:
    impl_->textFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    break;
  case AlignMiddle:
    impl_->textFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    break;
  case AlignBottom:
    impl_->textFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_FAR);
    break;
  }
  
  switch (horizontalAlign) {
  default:
  case AlignLeft:
    impl_->textFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    break;
  case AlignCenter:
    impl_->textFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    break;
  case AlignRight:
    impl_->textFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    break;
  }

  impl_->rt_->DrawText(txt.c_str(),
    txt.size(), impl_->textFormat_, &textRect, impl_->strokeBrush_, D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);
}

WTextItem WRasterImage::measureText(const WString& text, double maxWidth,
				    bool wordWrap)
{
  HRESULT hr = S_OK;
  std::wstring txt = text;
  IDWriteTextLayout *textLayout = NULL;
  hr = impl_->writeFactory_->CreateTextLayout(
    txt.c_str(),
    txt.size(),
    impl_->textFormat_,
    maxWidth == -1.0 ?
      std::numeric_limits<FLOAT>::infinity() :
      static_cast<FLOAT>(maxWidth),
    std::numeric_limits<FLOAT>::infinity(),
    &textLayout);
  DWRITE_TEXT_METRICS textMetrics;
  hr = textLayout->GetMetrics(&textMetrics);
  SafeRelease(textLayout);
  return WTextItem(text, textMetrics.width);
}

WFontMetrics WRasterImage::fontMetrics()
{
  DWRITE_FONT_METRICS metrics;
  impl_->font_->GetMetrics(&metrics);
  double unitsPerEm = metrics.designUnitsPerEm;
  double ems = impl_->textFormat_->GetFontSize();
  double pxs = painter()->font().sizeLength().toPixels();
  double pxsPerEm = pxs / ems;
  double ascent = metrics.ascent / unitsPerEm * pxsPerEm;
  double descent = metrics.descent / unitsPerEm * pxsPerEm;
  double leading = metrics.lineGap / unitsPerEm * pxsPerEm;
  return WFontMetrics(painter()->font(), leading, ascent, descent);
}

class IStreamToOStream : public IStream
{
  public:
  IStreamToOStream(std::ostream &os) :
    os_(os),
    refcount_(1)
  {
  }

  virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void ** ppvObject)
  {
    if (iid == __uuidof(IUnknown)
	|| iid == __uuidof(IStream)
	|| iid == __uuidof(ISequentialStream)) {
      *ppvObject = static_cast<IStream*>(this);
      AddRef();
      return S_OK;
    } else
      return E_NOINTERFACE;
  }

  virtual ULONG STDMETHODCALLTYPE AddRef(void)
  {
    return (ULONG)InterlockedIncrement(&refcount_);
  }

  virtual ULONG STDMETHODCALLTYPE Release(void)
  {
    ULONG res = (ULONG)InterlockedDecrement(&refcount_);
    if (res == 0)
      delete this;
    return res;
  }

  virtual HRESULT STDMETHODCALLTYPE Read(void* pv, ULONG cb, ULONG* pcbRead)
  {
    return E_NOTIMPL;
  }

  virtual HRESULT STDMETHODCALLTYPE Write(void const *pv, ULONG cb, ULONG *pcbWritten)
  {
    os_.write((const char *)pv, cb);
    if (pcbWritten)
      *pcbWritten = cb;
    return S_OK;
  }

  virtual HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER)
  {
    return E_NOTIMPL;
  }

  virtual HRESULT STDMETHODCALLTYPE CopyTo(IStream*, ULARGE_INTEGER, ULARGE_INTEGER*,
					   ULARGE_INTEGER*)
  {
    return E_NOTIMPL;
  }

  virtual HRESULT STDMETHODCALLTYPE Commit(DWORD)
  {
    return E_NOTIMPL;
  }

  virtual HRESULT STDMETHODCALLTYPE Revert(void)
  {
    return E_NOTIMPL;
  }

  virtual HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD)
  {
    return E_NOTIMPL;
  }

  virtual HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD)
  {
    return E_NOTIMPL;
  }

  virtual HRESULT STDMETHODCALLTYPE Clone(IStream **)
  {
    return E_NOTIMPL;
  }

  virtual HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER liDistanceToMove, DWORD dwOrigin,
					 ULARGE_INTEGER* lpNewFilePointer)
  {
    return E_NOTIMPL;
  }

  virtual HRESULT STDMETHODCALLTYPE Stat(STATSTG* pStatstg, DWORD grfStatFlag)
  {
    return E_NOTIMPL;
  }

private:
  std::ostream &os_;
  LONG refcount_;
};

void WRasterImage::handleRequest(const Http::Request& request,
				 Http::Response& response)
{
  response.setMimeType("image/" + impl_->type_);

  HRESULT hr = S_OK;

  IWICBitmapEncoder *encoder = NULL;
  IWICBitmapFrameEncode *frameEncode = NULL;

  WICPixelFormatGUID format = GUID_WICPixelFormat32bppPRGBA;

  IStreamToOStream *istream = new IStreamToOStream(response.out());
  if (SUCCEEDED(hr)) {
    hr = impl_->wicFactory_->CreateEncoder(
      impl_->type_ == "png" ? GUID_ContainerFormatPng :
      impl_->type_ == "jpg" ? GUID_ContainerFormatJpeg :
      GUID_ContainerFormatPng, NULL, &encoder);
  }
  if (SUCCEEDED(hr)) {
    hr = encoder->Initialize(istream, WICBitmapEncoderNoCache);
  }
  if (SUCCEEDED(hr)) {
    hr = encoder->CreateNewFrame(&frameEncode, NULL);
  }
  if (SUCCEEDED(hr)) {
    hr = frameEncode->Initialize(NULL);
  }

  if (SUCCEEDED(hr)) {
    hr = frameEncode->SetSize(impl_->w_, impl_->h_);
  }
  if (SUCCEEDED(hr)) {
    hr = frameEncode->SetPixelFormat(&format);
  }
  if (SUCCEEDED(hr)) {
    hr = frameEncode->WriteSource(impl_->bitmap_, NULL);
  }
  if (SUCCEEDED(hr)) {
    hr = frameEncode->Commit();
  }
  if (SUCCEEDED(hr)) {
    hr = encoder->Commit();
  }
  if (!SUCCEEDED(hr)) {
    LOG_ERROR("Error while serving raster image resource: HRESULT " << hr);
    response.setStatus(500);
  }

  SafeRelease(frameEncode);
  SafeRelease(encoder);
  SafeRelease(istream);
}

}
