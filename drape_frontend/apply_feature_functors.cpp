#include "drape_frontend/apply_feature_functors.hpp"

#include "drape_frontend/area_shape.hpp"
#include "drape_frontend/color_constants.hpp"
#include "drape_frontend/colored_symbol_shape.hpp"
#include "drape_frontend/line_shape.hpp"
#include "drape_frontend/path_symbol_shape.hpp"
#include "drape_frontend/path_text_shape.hpp"
#include "drape_frontend/poi_symbol_shape.hpp"
#include "drape_frontend/text_layout.hpp"
#include "drape_frontend/text_shape.hpp"
#include "drape_frontend/visual_params.hpp"

#include "indexer/drawing_rules.hpp"
#include "indexer/drules_include.hpp"
#include "indexer/map_style_reader.hpp"
#include "indexer/osm_editor.hpp"
#include "indexer/road_shields_parser.hpp"

#include "geometry/clipping.hpp"

#include "drape/color.hpp"
#include "drape/stipple_pen_resource.hpp"
#include "drape/texture_manager.hpp"
#include "drape/utils/projection.hpp"

#include "base/logging.hpp"
#include "base/stl_helpers.hpp"

#include <algorithm>
#include <limits>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace df
{

dp::Color ToDrapeColor(uint32_t src)
{
  return dp::Extract(src, static_cast<uint8_t>(255 - (src >> 24)));
}

namespace
{
double const kMinVisibleFontSize = 8.0;

std::string const kStarSymbol = "★";
std::string const kPriceSymbol = "$";

df::ColorConstant const kPoiHotelTextOutlineColor = "PoiHotelTextOutline";
df::ColorConstant const kRoadShieldBlackTextColor = "RoadShieldBlackText";
df::ColorConstant const kRoadShieldWhiteTextColor = "RoadShieldWhiteText";
df::ColorConstant const kRoadShieldUKYellowTextColor = "RoadShieldUKYellowText";
df::ColorConstant const kRoadShieldGreenBackgroundColor = "RoadShieldGreenBackground";
df::ColorConstant const kRoadShieldBlueBackgroundColor = "RoadShieldBlueBackground";
df::ColorConstant const kRoadShieldRedBackgroundColor = "RoadShieldRedBackground";
df::ColorConstant const kRoadShieldOrangeBackgroundColor = "RoadShieldOrangeBackground";

int const kLineSimplifyLevelStart = 10;
int const kLineSimplifyLevelEnd = 12;

uint32_t const kPathTextBaseTextIndex = 128;
uint32_t const kShieldBaseTextIndex = 0;

#ifdef CALC_FILTERED_POINTS
class LinesStat
{
public:
  ~LinesStat()
  {
    map<int, TValue> zoomValues;
    for (pair<TKey, TValue> const & f : m_features)
    {
      TValue & v = zoomValues[f.first.second];
      v.m_neededPoints += f.second.m_neededPoints;
      v.m_readedPoints += f.second.m_readedPoints;
    }

    LOG(LINFO, ("========================"));
    for (pair<int, TValue> const & v : zoomValues)
      LOG(LINFO, ("Zoom = ", v.first, " Percent = ", 1 - v.second.m_neededPoints / (double)v.second.m_readedPoints));
  }

  static LinesStat & Get()
  {
    static LinesStat s_stat;
    return s_stat;
  }

  void InsertLine(FeatureID const & id, double scale, int vertexCount, int renderVertexCount)
  {
    int s = 0;
    double factor = 5.688;
    while (factor < scale)
    {
      s++;
      factor = factor * 2.0;
    }

    InsertLine(id, s, vertexCount, renderVertexCount);
  }

  void InsertLine(FeatureID const & id, int scale, int vertexCount, int renderVertexCount)
  {
    TKey key(id, scale);
    lock_guard<mutex> g(m_mutex);
    if (m_features.find(key) != m_features.end())
      return;

    TValue & v = m_features[key];
    v.m_readedPoints = vertexCount;
    v.m_neededPoints = renderVertexCount;
  }

private:
  LinesStat() = default;

  using TKey = pair<FeatureID, int>;
  struct TValue
  {
    int m_readedPoints = 0;
    int m_neededPoints = 0;
  };

  map<TKey, TValue> m_features;
  mutex m_mutex;
};
#endif

void Extract(::LineDefProto const * lineRule, df::LineViewParams & params)
{
  double const scale = df::VisualParams::Instance().GetVisualScale();
  params.m_color = ToDrapeColor(lineRule->color());
  params.m_width = static_cast<float>(std::max(lineRule->width() * scale, 1.0));

  if (lineRule->has_dashdot())
  {
    DashDotProto const & dd = lineRule->dashdot();

    int const count = dd.dd_size();
    params.m_pattern.reserve(count);
    for (int i = 0; i < count; ++i)
      params.m_pattern.push_back(static_cast<uint8_t>(dd.dd(i) * scale));
  }

  switch(lineRule->cap())
  {
  case ::ROUNDCAP : params.m_cap = dp::RoundCap;
    break;
  case ::BUTTCAP  : params.m_cap = dp::ButtCap;
    break;
  case ::SQUARECAP: params.m_cap = dp::SquareCap;
    break;
  default:
    ASSERT(false, ());
  }

  switch (lineRule->join())
  {
  case ::NOJOIN    : params.m_join = dp::MiterJoin;
    break;
  case ::ROUNDJOIN : params.m_join = dp::RoundJoin;
    break;
  case ::BEVELJOIN : params.m_join = dp::BevelJoin;
    break;
  default:
    ASSERT(false, ());
  }
}

void CaptionDefProtoToFontDecl(CaptionDefProto const * capRule, dp::FontDecl & params)
{
  double const vs = df::VisualParams::Instance().GetVisualScale();
  params.m_color = ToDrapeColor(capRule->color());
  params.m_size = static_cast<float>(std::max(kMinVisibleFontSize, capRule->height() * vs));

  if (capRule->has_stroke_color())
    params.m_outlineColor = ToDrapeColor(capRule->stroke_color());
  else if (vs < df::VisualParams::kHdpiScale)
    params.m_isSdf = false;
}

void ShieldRuleProtoToFontDecl(ShieldRuleProto const * shieldRule, dp::FontDecl & params)
{
  double const vs = df::VisualParams::Instance().GetVisualScale();
  params.m_color = ToDrapeColor(shieldRule->text_color());
  params.m_size = static_cast<float>(std::max(kMinVisibleFontSize, shieldRule->height() * vs));
  if (shieldRule->has_text_stroke_color())
   params.m_outlineColor = ToDrapeColor(shieldRule->text_stroke_color());

  if (vs < df::VisualParams::kHdpiScale)
    params.m_isSdf = false;
}

dp::Anchor GetAnchor(CaptionDefProto const * capRule)
{
  if (capRule->has_offset_y())
  {
    if (capRule->offset_y() > 0)
      return dp::Top;
    else
      return dp::Bottom;
  }
  if (capRule->has_offset_x())
  {
    if (capRule->offset_x() > 0)
      return dp::Left;
    else
      return dp::Right;
  }

  return dp::Center;
}

m2::PointF GetOffset(CaptionDefProto const * capRule)
{
  double const vs = VisualParams::Instance().GetVisualScale();
  m2::PointF result(0, 0);
  if (capRule != nullptr && capRule->has_offset_x())
    result.x = static_cast<float>(capRule->offset_x() * vs);
  if (capRule != nullptr && capRule->has_offset_y())
    result.y = static_cast<float>(capRule->offset_y() * vs);

  return result;
}

uint16_t CalculateHotelOverlayPriority(BaseApplyFeature::HotelData const & data)
{
  // NOTE: m_rating is in format X[.Y], where X = [0;10], Y = [0;9], e.g. 8.7
  string s = data.m_rating;
  s.erase(remove(s.begin(), s.end(), '.'), s.end());
  s.erase(remove(s.begin(), s.end(), ','), s.end());
  if (s.empty())
    return 0;

  // Special case for integer ratings.
  if (s.length() == data.m_rating.length())
    s += '0';

  uint result = 0;
  if (strings::to_uint(s, result))
    return static_cast<uint16_t>(result);
  return 0;
}

uint16_t CalculateNavigationPoiPriority()
{
  // All navigation POI have maximum priority in navigation mode.
  return std::numeric_limits<uint16_t>::max();
}

uint16_t CalculateNavigationRoadShieldPriority()
{
  // Road shields have less priority than navigation POI.
  static uint16_t constexpr kMask = ~static_cast<uint16_t>(0xFF);
  uint16_t priority = CalculateNavigationPoiPriority();
  return priority & kMask;
}

uint16_t CalculateNavigationPathTextPriority(uint32_t textIndex)
{
  // Path texts have more priority than road shields in navigation mode.
  static uint16_t constexpr kMask = ~static_cast<uint16_t>(0xFF);
  uint16_t priority = CalculateNavigationPoiPriority();
  priority &= kMask;

  uint8_t constexpr kMaxTextIndex = std::numeric_limits<uint8_t>::max() - 1;
  if (textIndex > kMaxTextIndex)
    textIndex = kMaxTextIndex;
  priority |= static_cast<uint8_t>(textIndex);
  return priority;
}

bool IsSymbolRoadShield(ftypes::RoadShield const & shield)
{
  return shield.m_type == ftypes::RoadShieldType::US_Interstate ||
         shield.m_type == ftypes::RoadShieldType::US_Highway;
}

std::string GetRoadShieldSymbolName(ftypes::RoadShield const & shield, double fontScale)
{
  std::string result = "";
  if (shield.m_type == ftypes::RoadShieldType::US_Interstate)
    result = shield.m_name.size() <= 2 ? "shield-us-i-thin" : "shield-us-i-wide";
  else if (shield.m_type == ftypes::RoadShieldType::US_Highway)
    result = shield.m_name.size() <= 2 ? "shield-us-hw-thin" : "shield-us-hw-wide";

  if (!result.empty() && fontScale > 1.0)
    result += "-scaled";

  return result;
}

bool IsColoredRoadShield(ftypes::RoadShield const & shield)
{
  return shield.m_type == ftypes::RoadShieldType::Default ||
         shield.m_type == ftypes::RoadShieldType::UK_Highway ||
         shield.m_type == ftypes::RoadShieldType::Generic_White ||
         shield.m_type == ftypes::RoadShieldType::Generic_Blue ||
         shield.m_type == ftypes::RoadShieldType::Generic_Green ||
         shield.m_type == ftypes::RoadShieldType::Generic_Red ||
         shield.m_type == ftypes::RoadShieldType::Generic_Orange;
}

dp::FontDecl GetRoadShieldTextFont(dp::FontDecl const & baseFont, ftypes::RoadShield const & shield)
{
  dp::FontDecl f = baseFont;
  f.m_outlineColor = dp::Color::Transparent();

  using ftypes::RoadShieldType;

  static std::unordered_map<int, df::ColorConstant> kColors = {
      {my::Key(RoadShieldType::Generic_Green), kRoadShieldWhiteTextColor},
      {my::Key(RoadShieldType::Generic_Blue), kRoadShieldWhiteTextColor},
      {my::Key(RoadShieldType::UK_Highway), kRoadShieldUKYellowTextColor},
      {my::Key(RoadShieldType::US_Interstate), kRoadShieldWhiteTextColor},
      {my::Key(RoadShieldType::US_Highway), kRoadShieldBlackTextColor},
      {my::Key(RoadShieldType::Generic_Red), kRoadShieldWhiteTextColor},
      {my::Key(RoadShieldType::Generic_Orange), kRoadShieldBlackTextColor}
  };

  auto it = kColors.find(my::Key(shield.m_type));
  if (it != kColors.end())
    f.m_color = df::GetColorConstant(it->second);

  return f;
}

dp::Color GetRoadShieldColor(dp::Color const & baseColor, ftypes::RoadShield const & shield)
{
  using ftypes::RoadShieldType;

  static std::unordered_map<int, df::ColorConstant> kColors = {
      {my::Key(RoadShieldType::Generic_Green), kRoadShieldGreenBackgroundColor},
      {my::Key(RoadShieldType::Generic_Blue), kRoadShieldBlueBackgroundColor},
      {my::Key(RoadShieldType::UK_Highway), kRoadShieldGreenBackgroundColor},
      {my::Key(RoadShieldType::Generic_Red), kRoadShieldRedBackgroundColor},
      {my::Key(RoadShieldType::Generic_Orange), kRoadShieldOrangeBackgroundColor}
  };

  auto it = kColors.find(my::Key(shield.m_type));
  if (it != kColors.end())
    return df::GetColorConstant(it->second);

  return baseColor;
}

float GetRoadShieldOutlineWidth(float baseWidth, ftypes::RoadShield const & shield)
{
  if (shield.m_type == ftypes::RoadShieldType::UK_Highway ||
      shield.m_type == ftypes::RoadShieldType::Generic_Blue ||
      shield.m_type == ftypes::RoadShieldType::Generic_Green ||
      shield.m_type == ftypes::RoadShieldType::Generic_Red)
    return 0.0f;

  return baseWidth;
}

dp::Anchor GetShieldAnchor(uint8_t shieldIndex, uint8_t shieldCount)
{
  switch (shieldCount)
  {
    case 2:
      if (shieldIndex == 0) return dp::Bottom;
      return dp::Top;
    case 3:
      if (shieldIndex == 0) return dp::RightBottom;
      else if (shieldIndex == 1) return dp::LeftBottom;
      return dp::Top;
    case 4:
      if (shieldIndex == 0) return dp::RightBottom;
      else if (shieldIndex == 1) return dp::LeftBottom;
      else if (shieldIndex == 2) return dp::RightTop;
      return dp::LeftTop;
  }
  return dp::Center;
}

m2::PointF GetShieldOffset(dp::Anchor anchor, double borderWidth, double borderHeight)
{
  m2::PointF offset(0.0f, 0.0f);
  if (anchor & dp::Left)
    offset.x = static_cast<float>(borderWidth);
  else if (anchor & dp::Right)
    offset.x = -static_cast<float>(borderWidth);

  if (anchor & dp::Top)
    offset.y = static_cast<float>(borderHeight);
  else if (anchor & dp::Bottom)
    offset.y = -static_cast<float>(borderHeight);
  return offset;
}

void CalculateRoadShieldPositions(std::vector<float> const & offsets,
                                  m2::SharedSpline const & spline,
                                  std::vector<m2::PointD> & shieldPositions)
{
  ASSERT(!offsets.empty(), ());
  if (offsets.size() == 1)
  {
    shieldPositions.push_back(spline->GetPoint(0.5 * spline->GetLength()).m_pos);
  }
  else
  {
    for (size_t i = 0; i + 1 < offsets.size(); i++)
    {
      double const p = 0.5 * (offsets[i] + offsets[i + 1]);
      shieldPositions.push_back(spline->GetPoint(p).m_pos);
    }
  }
}
}  // namespace

BaseApplyFeature::BaseApplyFeature(TileKey const & tileKey, TInsertShapeFn const & insertShape,
                                   FeatureID const & id, int minVisibleScale, uint8_t rank,
                                   CaptionDescription const & captions)
  : m_insertShape(insertShape)
  , m_id(id)
  , m_captions(captions)
  , m_minVisibleScale(minVisibleScale)
  , m_rank(rank)
  , m_tileKey(tileKey)
  , m_tileRect(tileKey.GetGlobalRect())
{
  ASSERT(m_insertShape != nullptr, ());
}

void BaseApplyFeature::ExtractCaptionParams(CaptionDefProto const * primaryProto,
                                            CaptionDefProto const * secondaryProto,
                                            float depth, TextViewParams & params) const
{
  dp::FontDecl decl;
  CaptionDefProtoToFontDecl(primaryProto, decl);

  params.m_anchor = GetAnchor(primaryProto);
  params.m_depth = depth;
  params.m_featureID = m_id;
  params.m_primaryText = m_captions.GetMainText();
  params.m_primaryTextFont = decl;
  params.m_primaryOffset = GetOffset(primaryProto);
  params.m_primaryOptional = primaryProto->has_is_optional() ? primaryProto->is_optional() : true;
  params.m_secondaryOptional = true;

  if (secondaryProto)
  {
    dp::FontDecl auxDecl;
    CaptionDefProtoToFontDecl(secondaryProto, auxDecl);

    params.m_secondaryText = m_captions.GetAuxText();
    params.m_secondaryTextFont = auxDecl;
    params.m_secondaryOptional = secondaryProto->has_is_optional() ? secondaryProto->is_optional() : true;
  }
}

string BaseApplyFeature::ExtractHotelInfo() const
{
  if (!m_hotelData.m_isHotel)
    return "";

  ostringstream out;
  if (!m_hotelData.m_rating.empty() && m_hotelData.m_rating != "0")
  {
    out << m_hotelData.m_rating << kStarSymbol;
    if (m_hotelData.m_priceCategory != 0)
      out << "  ";
  }
  for (int i = 0; i < m_hotelData.m_priceCategory; i++)
    out << kPriceSymbol;

  return out.str();
}

void BaseApplyFeature::SetHotelData(HotelData && hotelData)
{
  m_hotelData = move(hotelData);
}

ApplyPointFeature::ApplyPointFeature(TileKey const & tileKey, TInsertShapeFn const & insertShape,
                                     FeatureID const & id, int minVisibleScale, uint8_t rank,
                                     CaptionDescription const & captions, float posZ,
                                     int displacementMode, dp::GLState::DepthLayer depthLayer)
  : TBase(tileKey, insertShape, id, minVisibleScale, rank, captions)
  , m_posZ(posZ)
  , m_hasPoint(false)
  , m_hasArea(false)
  , m_createdByEditor(false)
  , m_obsoleteInEditor(false)
  , m_depthLayer(depthLayer)
  , m_symbolDepth(dp::minDepth)
  , m_symbolRule(nullptr)
  , m_displacementMode(displacementMode)
{}

void ApplyPointFeature::operator()(m2::PointD const & point, bool hasArea)
{
  auto const & editor = osm::Editor::Instance();
  m_hasPoint = true;
  m_hasArea = hasArea;
  auto const featureStatus = editor.GetFeatureStatus(m_id);
  m_createdByEditor = featureStatus == osm::Editor::FeatureStatus::Created;
  m_obsoleteInEditor = featureStatus == osm::Editor::FeatureStatus::Obsolete;
  m_centerPoint = point;
}

void ApplyPointFeature::ProcessPointRule(Stylist::TRuleWrapper const & rule)
{
  if (!m_hasPoint)
    return;

  drule::BaseRule const * pRule = rule.first;
  float const depth = static_cast<float>(rule.second);

  SymbolRuleProto const * symRule = pRule->GetSymbol();
  if (symRule != nullptr)
  {
    m_symbolDepth = depth;
    m_symbolRule = symRule;
  }

  bool const isNode = (pRule->GetType() & drule::node) != 0;
  CaptionDefProto const * capRule = pRule->GetCaption(0);
  if (capRule && isNode)
  {
    TextViewParams params;
    params.m_tileCenter = m_tileRect.Center();
    ExtractCaptionParams(capRule, pRule->GetCaption(1), depth, params);
    params.m_depthLayer = m_depthLayer;
    params.m_minVisibleScale = m_minVisibleScale;
    params.m_rank = m_rank;
    params.m_posZ = m_posZ;
    params.m_hasArea = m_hasArea;
    params.m_createdByEditor = m_createdByEditor;

    if (m_displacementMode == dp::displacement::kHotelMode &&
        m_hotelData.m_isHotel && !params.m_primaryText.empty())
    {
      params.m_primaryOptional = false;
      params.m_primaryTextFont.m_size *= 1.2;
      params.m_primaryTextFont.m_outlineColor = df::GetColorConstant(df::kPoiHotelTextOutlineColor);
      params.m_secondaryTextFont = params.m_primaryTextFont;
      params.m_secondaryText = ExtractHotelInfo();
      params.m_secondaryOptional = false;
    }

    if (!params.m_primaryText.empty() || !params.m_secondaryText.empty())
      m_textParams.push_back(params);
  }
}

void ApplyPointFeature::Finish(ref_ptr<dp::TextureManager> texMng,
                               CustomSymbolsContextPtr const & customSymbolsContext)
{
  m2::PointF symbolSize(0.0f, 0.0f);

  bool specialDisplacementMode = false;
  uint16_t specialModePriority = 0;
  if (m_displacementMode == dp::displacement::kHotelMode && m_hotelData.m_isHotel)
  {
    specialDisplacementMode = true;
    specialModePriority = CalculateHotelOverlayPriority(m_hotelData);
  }
  else if (m_depthLayer == dp::GLState::NavigationLayer && GetStyleReader().IsCarNavigationStyle())
  {
    specialDisplacementMode = true;
    specialModePriority = CalculateNavigationPoiPriority();
  }

  bool const hasPOI = m_symbolRule != nullptr;

  if (hasPOI)
  {
    PoiSymbolViewParams params(m_id);
    params.m_tileCenter = m_tileRect.Center();
    params.m_depth = static_cast<float>(m_symbolDepth);
    params.m_depthLayer = m_depthLayer;
    params.m_minVisibleScale = m_minVisibleScale;
    params.m_rank = m_rank;

    params.m_symbolName = m_symbolRule->name();
    bool prioritized = false;
    if (customSymbolsContext)
    {
      auto customSymbolIt = customSymbolsContext->m_symbols.find(m_id);
      if (customSymbolIt != customSymbolsContext->m_symbols.end())
      {
        params.m_symbolName = customSymbolIt->second.m_symbolName;
        prioritized = customSymbolIt->second.m_prioritized;
      }
    }

    double const mainScale = df::VisualParams::Instance().GetVisualScale();
    params.m_extendingSize = static_cast<uint32_t>(m_symbolRule->has_min_distance() ?
                                                   mainScale * m_symbolRule->min_distance() : 0.0);
    params.m_posZ = m_posZ;
    params.m_hasArea = m_hasArea;
    params.m_prioritized = prioritized || m_createdByEditor;
    params.m_obsoleteInEditor = m_obsoleteInEditor;
    params.m_specialDisplacementMode = specialDisplacementMode;
    params.m_specialModePriority = specialModePriority;

    m_insertShape(make_unique_dp<PoiSymbolShape>(m_centerPoint, params, m_tileKey, 0 /* text index */));

    dp::TextureManager::SymbolRegion region;
    texMng->GetSymbolRegion(params.m_symbolName, region);
    symbolSize = region.GetPixelSize();
  }

  for (auto textParams : m_textParams)
  {
    textParams.m_specialDisplacementMode = specialDisplacementMode;
    textParams.m_specialModePriority = specialModePriority;
    m_insertShape(make_unique_dp<TextShape>(m_centerPoint, textParams, m_tileKey,
                                            hasPOI, symbolSize, 0 /* textIndex */));
  }
}

ApplyAreaFeature::ApplyAreaFeature(TileKey const & tileKey, TInsertShapeFn const & insertShape,
                                   FeatureID const & id, double currentScaleGtoP, bool isBuilding,
                                   bool skipAreaGeometry, float minPosZ, float posZ, int minVisibleScale,
                                   uint8_t rank, CaptionDescription const & captions, bool hatchingArea)
  : TBase(tileKey, insertShape, id, minVisibleScale, rank, captions, posZ,
          dp::displacement::kDefaultMode, dp::GLState::GeometryLayer)
  , m_minPosZ(minPosZ)
  , m_isBuilding(isBuilding)
  , m_skipAreaGeometry(skipAreaGeometry)
  , m_hatchingArea(hatchingArea)
  , m_currentScaleGtoP(currentScaleGtoP)
{}

void ApplyAreaFeature::operator()(m2::PointD const & p1, m2::PointD const & p2, m2::PointD const & p3)
{
  if (m_isBuilding)
  {
    if (!m_skipAreaGeometry)
      ProcessBuildingPolygon(p1, p2, p3);
    return;
  }

  m2::PointD const v1 = p2 - p1;
  m2::PointD const v2 = p3 - p1;
  if (v1.IsAlmostZero() || v2.IsAlmostZero())
    return;

  double const crossProduct = m2::CrossProduct(v1.Normalize(), v2.Normalize());
  double const kEps = 1e-7;
  if (fabs(crossProduct) < kEps)
    return;

  auto const clipFunctor = [this](m2::PointD const & p1, m2::PointD const & p2, m2::PointD const & p3)
  {
    m_triangles.push_back(p1);
    m_triangles.push_back(p2);
    m_triangles.push_back(p3);
  };

  if (m2::CrossProduct(p2 - p1, p3 - p1) < 0)
    m2::ClipTriangleByRect(m_tileRect, p1, p2, p3, clipFunctor);
  else
    m2::ClipTriangleByRect(m_tileRect, p1, p3, p2, clipFunctor);
}

void ApplyAreaFeature::ProcessBuildingPolygon(m2::PointD const & p1, m2::PointD const & p2,
                                              m2::PointD const & p3)
{
  // For building we must filter degenerate polygons because now we have to reconstruct
  // building outline by bunch of polygons.
  m2::PointD const v1 = p2 - p1;
  m2::PointD const v2 = p3 - p1;
  if (v1.IsAlmostZero() || v2.IsAlmostZero())
    return;

  double const crossProduct = m2::CrossProduct(v1.Normalize(), v2.Normalize());
  double const kEps = 0.01;
  if (fabs(crossProduct) < kEps)
    return;

  if (crossProduct < 0)
  {
    m_triangles.push_back(p1);
    m_triangles.push_back(p2);
    m_triangles.push_back(p3);
    BuildEdges(GetIndex(p1), GetIndex(p2), GetIndex(p3));
  }
  else
  {
    m_triangles.push_back(p1);
    m_triangles.push_back(p3);
    m_triangles.push_back(p2);
    BuildEdges(GetIndex(p1), GetIndex(p3), GetIndex(p2));
  }
}

int ApplyAreaFeature::GetIndex(m2::PointD const & pt)
{
  for (size_t i = 0; i < m_points.size(); i++)
  {
    if (pt.EqualDxDy(m_points[i], 1e-7))
      return static_cast<int>(i);
  }
  m_points.push_back(pt);
  return static_cast<int>(m_points.size()) - 1;
}

bool ApplyAreaFeature::EqualEdges(TEdge const & edge1, TEdge const & edge2) const
{
  return (edge1.first == edge2.first && edge1.second == edge2.second) ||
         (edge1.first == edge2.second && edge1.second == edge2.first);
}

bool ApplyAreaFeature::FindEdge(TEdge const & edge)
{
  for (size_t i = 0; i < m_edges.size(); i++)
  {
    if (EqualEdges(m_edges[i].first, edge))
    {
      m_edges[i].second = -1;
      return true;
    }
  }
  return false;
}

m2::PointD ApplyAreaFeature::CalculateNormal(m2::PointD const & p1, m2::PointD const & p2,
                                             m2::PointD const & p3) const
{
  m2::PointD const tangent = (p2 - p1).Normalize();
  m2::PointD normal = m2::PointD(-tangent.y, tangent.x);
  m2::PointD const v = ((p1 + p2) * 0.5 - p3).Normalize();
  if (m2::DotProduct(normal, v) < 0.0)
    normal = -normal;

  return normal;
}

void ApplyAreaFeature::BuildEdges(int vertexIndex1, int vertexIndex2, int vertexIndex3)
{
  // Check if triangle is degenerate.
  if (vertexIndex1 == vertexIndex2 || vertexIndex2 == vertexIndex3 || vertexIndex1 == vertexIndex3)
    return;

  TEdge edge1 = make_pair(vertexIndex1, vertexIndex2);
  if (!FindEdge(edge1))
    m_edges.push_back(make_pair(move(edge1), vertexIndex3));

  TEdge edge2 = make_pair(vertexIndex2, vertexIndex3);
  if (!FindEdge(edge2))
    m_edges.push_back(make_pair(move(edge2), vertexIndex1));

  TEdge edge3 = make_pair(vertexIndex3, vertexIndex1);
  if (!FindEdge(edge3))
    m_edges.push_back(make_pair(move(edge3), vertexIndex2));
}

void ApplyAreaFeature::CalculateBuildingOutline(bool calculateNormals, BuildingOutline & outline)
{
  outline.m_vertices = move(m_points);
  outline.m_indices.reserve(m_edges.size() * 2);
  if (calculateNormals)
    outline.m_normals.reserve(m_edges.size());

  for (auto & e : m_edges)
  {
    if (e.second < 0)
      continue;

    outline.m_indices.push_back(e.first.first);
    outline.m_indices.push_back(e.first.second);

    if (calculateNormals)
    {
      outline.m_normals.emplace_back(CalculateNormal(outline.m_vertices[e.first.first],
                                                     outline.m_vertices[e.first.second],
                                                     outline.m_vertices[e.second]));
    }
  }
}

void ApplyAreaFeature::ProcessAreaRule(Stylist::TRuleWrapper const & rule)
{
  drule::BaseRule const * pRule = rule.first;
  float const depth = static_cast<float>(rule.second);

  AreaRuleProto const * areaRule = pRule->GetArea();
  if (areaRule && !m_triangles.empty())
  {
    AreaViewParams params;
    params.m_tileCenter = m_tileRect.Center();
    params.m_depth = depth;
    params.m_color = ToDrapeColor(areaRule->color());
    params.m_minVisibleScale = m_minVisibleScale;
    params.m_rank = m_rank;
    params.m_minPosZ = m_minPosZ;
    params.m_posZ = m_posZ;
    params.m_hatching = m_hatchingArea;
    params.m_baseGtoPScale = static_cast<float>(m_currentScaleGtoP);

    BuildingOutline outline;
    if (m_isBuilding)
    {
      outline.m_generateOutline = areaRule->has_border() &&
                                  areaRule->color() != areaRule->border().color() &&
                                  areaRule->border().width() > 0.0;
      if (outline.m_generateOutline)
        params.m_outlineColor = ToDrapeColor(areaRule->border().color());
      bool const calculateNormals = m_posZ > 0.0;
      CalculateBuildingOutline(calculateNormals, outline);
      params.m_is3D = !outline.m_indices.empty() && calculateNormals;
    }

    m_insertShape(make_unique_dp<AreaShape>(move(m_triangles), move(outline), params));
  }
  else
  {
    TBase::ProcessPointRule(rule);
  }
}

ApplyLineFeatureGeometry::ApplyLineFeatureGeometry(TileKey const & tileKey,
                                                   TInsertShapeFn const & insertShape,
                                                   FeatureID const & id,
                                                   double currentScaleGtoP,
                                                   int minVisibleScale, uint8_t rank,
                                                   size_t pointsCount)
  : TBase(tileKey, insertShape, id, minVisibleScale, rank, CaptionDescription())
  , m_currentScaleGtoP(static_cast<float>(currentScaleGtoP))
  , m_sqrScale(math::sqr(currentScaleGtoP))
  , m_simplify(tileKey.m_zoomLevel >= kLineSimplifyLevelStart &&
               tileKey.m_zoomLevel <= kLineSimplifyLevelEnd)
  , m_initialPointsCount(pointsCount)
#ifdef CALC_FILTERED_POINTS
  , m_readedCount(0)
#endif
{}

void ApplyLineFeatureGeometry::operator() (m2::PointD const & point)
{
#ifdef CALC_FILTERED_POINTS
  ++m_readedCount;
#endif

  if (m_spline.IsNull())
    m_spline.Reset(new m2::Spline(m_initialPointsCount));

  if (m_spline->IsEmpty())
  {
    m_spline->AddPoint(point);
    m_lastAddedPoint = point;
  }
  else
  {
    static double minSegmentLength = math::sqr(4.0 * df::VisualParams::Instance().GetVisualScale());
    if (m_simplify &&
        ((m_spline->GetSize() > 1 && point.SquareLength(m_lastAddedPoint) * m_sqrScale < minSegmentLength) ||
        m_spline->IsPrelonging(point)))
    {
      m_spline->ReplacePoint(point);
    }
    else
    {
      m_spline->AddPoint(point);
      m_lastAddedPoint = point;
    }
  }
}

bool ApplyLineFeatureGeometry::HasGeometry() const
{
  return m_spline->IsValid();
}

void ApplyLineFeatureGeometry::ProcessLineRule(Stylist::TRuleWrapper const & rule)
{
  ASSERT(HasGeometry(), ());
  drule::BaseRule const * pRule = rule.first;
  float const depth = static_cast<float>(rule.second);

  LineDefProto const * pLineRule = pRule->GetLine();
  if (pLineRule == nullptr)
    return;

  m_clippedSplines = m2::ClipSplineByRect(m_tileRect, m_spline);
  if (m_clippedSplines.empty())
    return;

  if (pLineRule->has_pathsym())
  {
    PathSymProto const & symRule = pLineRule->pathsym();
    PathSymbolViewParams params;
    params.m_tileCenter = m_tileRect.Center();
    params.m_depth = depth;
    params.m_minVisibleScale = m_minVisibleScale;
    params.m_rank = m_rank;
    params.m_symbolName = symRule.name();
    double const mainScale = df::VisualParams::Instance().GetVisualScale();
    params.m_offset = static_cast<float>(symRule.offset() * mainScale);
    params.m_step = static_cast<float>(symRule.step() * mainScale);
    params.m_baseGtoPScale = m_currentScaleGtoP;

    for (auto const & spline : m_clippedSplines)
      m_insertShape(make_unique_dp<PathSymbolShape>(spline, params));
  }
  else
  {
    LineViewParams params;
    params.m_tileCenter = m_tileRect.Center();
    Extract(pLineRule, params);
    params.m_depth = depth;
    params.m_minVisibleScale = m_minVisibleScale;
    params.m_rank = m_rank;
    params.m_baseGtoPScale = m_currentScaleGtoP;
    params.m_zoomLevel = m_tileKey.m_zoomLevel;

    for (auto const & spline : m_clippedSplines)
      m_insertShape(make_unique_dp<LineShape>(spline, params));
  }
}

void ApplyLineFeatureGeometry::Finish()
{
#ifdef CALC_FILTERED_POINTS
  LinesStat::Get().InsertLine(m_id, m_currentScaleGtoP, m_readedCount, m_spline->GetSize());
#endif
}

ApplyLineFeatureAdditional::ApplyLineFeatureAdditional(TileKey const & tileKey,
                                                       TInsertShapeFn const & insertShape,
                                                       FeatureID const & id,
                                                       double currentScaleGtoP,
                                                       int minVisibleScale, uint8_t rank,
                                                       CaptionDescription const & captions,
                                                       std::vector<m2::SharedSpline> const & clippedSplines)
  : TBase(tileKey, insertShape, id, minVisibleScale, rank, captions)
  , m_clippedSplines(clippedSplines)
  , m_currentScaleGtoP(static_cast<float>(currentScaleGtoP))
  , m_depth(0.0f)
  , m_captionRule(nullptr)
  , m_shieldRule(nullptr)
{}

void ApplyLineFeatureAdditional::ProcessLineRule(Stylist::TRuleWrapper const & rule)
{
  if (m_clippedSplines.empty())
    return;

  drule::BaseRule const * pRule = rule.first;
  m_depth = static_cast<float>(rule.second);

  ShieldRuleProto const * pShieldRule = pRule->GetShield();
  if (pShieldRule != nullptr)
    m_shieldRule = pShieldRule;

  bool const isWay = (pRule->GetType() & drule::way) != 0;
  if (!isWay)
    return;

  CaptionDefProto const * pCaptionRule = pRule->GetCaption(0);
  if (pCaptionRule != nullptr && pCaptionRule->height() > 2 && !m_captions.GetMainText().empty())
    m_captionRule = pCaptionRule;
}

void ApplyLineFeatureAdditional::GetRoadShieldsViewParams(ref_ptr<dp::TextureManager> texMng,
                                                          ftypes::RoadShield const & shield,
                                                          uint8_t shieldIndex, uint8_t shieldCount,
                                                          TextViewParams & textParams,
                                                          ColoredSymbolViewParams & symbolParams,
                                                          PoiSymbolViewParams & poiParams,
                                                          m2::PointD & shieldPixelSize)
{
  ASSERT (m_shieldRule != nullptr, ());

  string const & roadNumber = shield.m_name;
  double const mainScale = df::VisualParams::Instance().GetVisualScale();
  double const fontScale = df::VisualParams::Instance().GetFontScale();
  auto const anchor = GetShieldAnchor(shieldIndex, shieldCount);
  m2::PointF const shieldOffset = GetShieldOffset(anchor, 2.0, 2.0);
  double const borderWidth = 5.0 * mainScale;
  double const borderHeight = 1.5 * mainScale;
  m2::PointF const shieldTextOffset = GetShieldOffset(anchor, borderWidth, borderHeight);

  // Text properties.
  dp::FontDecl baseFont;
  ShieldRuleProtoToFontDecl(m_shieldRule, baseFont);
  dp::FontDecl font = GetRoadShieldTextFont(baseFont, shield);
  textParams.m_tileCenter = m_tileRect.Center();
  textParams.m_depth = m_depth;
  textParams.m_depthLayer = dp::GLState::OverlayLayer;
  textParams.m_minVisibleScale = m_minVisibleScale;
  textParams.m_rank = m_rank;
  textParams.m_anchor = anchor;
  textParams.m_featureID = m_id;
  textParams.m_primaryText = roadNumber;
  textParams.m_primaryTextFont = font;
  textParams.m_primaryOffset = shieldOffset + shieldTextOffset;
  textParams.m_primaryOptional = false;
  textParams.m_secondaryOptional = false;
  textParams.m_extendingSize = 0;

  TextLayout textLayout;
  textLayout.Init(strings::MakeUniString(roadNumber), font.m_size, font.m_isSdf, texMng);

  // Calculate width and height of a shield.
  shieldPixelSize.x = textLayout.GetPixelLength() + 2.0 * borderWidth;
  shieldPixelSize.y = textLayout.GetPixelHeight() + 2.0 * borderHeight;
  textParams.m_limitedText = true;
  textParams.m_limits = shieldPixelSize * 0.9;

  bool needAdditionalText = false;

  if (IsColoredRoadShield(shield))
  {
    // Generated symbol properties.
    symbolParams.m_featureID = m_id;
    symbolParams.m_tileCenter = m_tileRect.Center();
    symbolParams.m_depth = m_depth;
    symbolParams.m_depthLayer = dp::GLState::OverlayLayer;
    symbolParams.m_minVisibleScale = m_minVisibleScale;
    symbolParams.m_rank = m_rank;
    symbolParams.m_anchor = anchor;
    symbolParams.m_offset = shieldOffset;
    symbolParams.m_shape = ColoredSymbolViewParams::Shape::RoundedRectangle;
    symbolParams.m_radiusInPixels = static_cast<float>(2.5 * mainScale);
    symbolParams.m_color = ToDrapeColor(m_shieldRule->color());
    if (m_shieldRule->has_stroke_color())
    {
      symbolParams.m_outlineColor = ToDrapeColor(m_shieldRule->stroke_color());
      symbolParams.m_outlineWidth = static_cast<float>(1.0 * mainScale);
    }
    symbolParams.m_sizeInPixels = shieldPixelSize;
    symbolParams.m_outlineWidth = GetRoadShieldOutlineWidth(symbolParams.m_outlineWidth, shield);
    symbolParams.m_color = GetRoadShieldColor(symbolParams.m_color, shield);

    needAdditionalText = !shield.m_additionalText.empty() &&
                         (anchor & dp::Top || anchor & dp::Center);
  }

  // Image symbol properties.
  if (IsSymbolRoadShield(shield))
  {
    std::string symbolName = GetRoadShieldSymbolName(shield, fontScale);
    poiParams.m_tileCenter = m_tileRect.Center();
    poiParams.m_depth = m_depth;
    poiParams.m_depthLayer = dp::GLState::OverlayLayer;
    poiParams.m_minVisibleScale = m_minVisibleScale;
    poiParams.m_rank = m_rank;
    poiParams.m_symbolName = symbolName;
    poiParams.m_extendingSize = 0;
    poiParams.m_posZ = 0.0f;
    poiParams.m_hasArea = false;
    poiParams.m_prioritized = false;
    poiParams.m_obsoleteInEditor = false;
    poiParams.m_anchor = anchor;
    poiParams.m_offset = GetShieldOffset(anchor, 0.5, 0.5);

    dp::TextureManager::SymbolRegion region;
    texMng->GetSymbolRegion(poiParams.m_symbolName, region);
    float const symBorderWidth = (region.GetPixelSize().x - textLayout.GetPixelLength()) * 0.5f;
    float const symBorderHeight = (region.GetPixelSize().y - textLayout.GetPixelHeight()) * 0.5f;
    textParams.m_primaryOffset = poiParams.m_offset + GetShieldOffset(anchor, symBorderWidth, symBorderHeight);
    shieldPixelSize = region.GetPixelSize();

    needAdditionalText = !symbolName.empty() && !shield.m_additionalText.empty() &&
                         (anchor & dp::Top || anchor & dp::Center);
  }

  if (needAdditionalText)
  {
    textParams.m_secondaryText = shield.m_additionalText;
    textParams.m_secondaryTextFont = textParams.m_primaryTextFont;
    textParams.m_secondaryTextFont.m_color = df::GetColorConstant(kRoadShieldBlackTextColor);
    textParams.m_secondaryTextFont.m_outlineColor = df::GetColorConstant(kRoadShieldWhiteTextColor);
    textParams.m_secondaryTextFont.m_size *= 0.9f;
    textParams.m_secondaryOffset = m2::PointD(0.0f, 3.0 * mainScale);
  }

  // Special priority for road shields in navigation style.
  if (GetStyleReader().IsCarNavigationStyle())
  {
    textParams.m_specialDisplacementMode = poiParams.m_specialDisplacementMode
      = symbolParams.m_specialDisplacementMode = true;
    textParams.m_specialModePriority = poiParams.m_specialModePriority
      = symbolParams.m_specialModePriority = CalculateNavigationRoadShieldPriority();
  }
}

bool ApplyLineFeatureAdditional::CheckShieldsNearby(m2::PointD const & shieldPos,
                                                    m2::PointD const & shieldPixelSize,
                                                    uint32_t minDistanceInPixels,
                                                    std::vector<m2::RectD> & shields)
{
  // Here we calculate extended rect to skip the same shields nearby.
  m2::PointD const skippingArea(2 * minDistanceInPixels, 2 * minDistanceInPixels);
  m2::PointD const extendedPixelSize = shieldPixelSize + skippingArea;
  m2::PointD const shieldMercatorHalfSize = extendedPixelSize / m_currentScaleGtoP * 0.5;
  m2::RectD shieldRect(shieldPos - shieldMercatorHalfSize, shieldPos + shieldMercatorHalfSize);
  for (auto const & r : shields)
  {
    if (r.IsIntersect(shieldRect))
      return false;
  }
  shields.push_back(shieldRect);
  return true;
}

void ApplyLineFeatureAdditional::Finish(ref_ptr<dp::TextureManager> texMng,
                                        std::set<ftypes::RoadShield> && roadShields,
                                        GeneratedRoadShields & generatedRoadShields)
{
  if (m_clippedSplines.empty())
    return;

  float const vs = static_cast<float>(df::VisualParams::Instance().GetVisualScale());

  std::vector<m2::PointD> shieldPositions;
  if (m_shieldRule != nullptr && !roadShields.empty())
    shieldPositions.reserve(m_clippedSplines.size() * 3);

  if (m_captionRule != nullptr)
  {
    dp::FontDecl fontDecl;
    CaptionDefProtoToFontDecl(m_captionRule, fontDecl);
    PathTextViewParams params;
    params.m_tileCenter = m_tileRect.Center();
    params.m_featureID = m_id;
    params.m_depth = m_depth;
    params.m_minVisibleScale = m_minVisibleScale;
    params.m_rank = m_rank;
    params.m_mainText = m_captions.GetMainText();
    params.m_auxText = m_captions.GetAuxText();
    params.m_textFont = fontDecl;
    params.m_baseGtoPScale = m_currentScaleGtoP;
    bool const navigationEnabled = GetStyleReader().IsCarNavigationStyle();
    if (navigationEnabled)
      params.m_specialDisplacementMode = true;

    uint32_t textIndex = kPathTextBaseTextIndex;
    for (auto const & spline : m_clippedSplines)
    {
      PathTextViewParams p = params;
      if (navigationEnabled)
        p.m_specialModePriority = CalculateNavigationPathTextPriority(textIndex);
      auto shape = make_unique_dp<PathTextShape>(spline, p, m_tileKey, textIndex);

      if (!shape->CalculateLayout(texMng))
        continue;

      if (m_shieldRule != nullptr && !roadShields.empty())
        CalculateRoadShieldPositions(shape->GetOffsets(), spline, shieldPositions);

      m_insertShape(std::move(shape));
      textIndex++;
    }
  }
  else if (m_shieldRule != nullptr && !roadShields.empty())
  {
    for (auto const & spline : m_clippedSplines)
    {
      float const pixelLength = 300.0f * vs;
      std::vector<float> offsets;
      PathTextLayout::CalculatePositions(static_cast<float>(spline->GetLength()),
                                         m_currentScaleGtoP, pixelLength, offsets);
      if (!offsets.empty())
        CalculateRoadShieldPositions(offsets, spline, shieldPositions);
    }
  }

  if (shieldPositions.empty())
    return;

  ASSERT(m_shieldRule != nullptr, ());
  int constexpr kDefaultMinDistance = 50;
  int minDistance = m_shieldRule->has_min_distance() ? m_shieldRule->min_distance()
                                                     : kDefaultMinDistance;
  if (minDistance < 0)
    minDistance = kDefaultMinDistance;
  uint32_t const scaledMinDistance = static_cast<uint32_t>(vs * minDistance);

  uint8_t shieldIndex = 0;
  uint32_t textIndex = kShieldBaseTextIndex;
  for (ftypes::RoadShield const & shield : roadShields)
  {
    TextViewParams textParams;
    ColoredSymbolViewParams symbolParams;
    PoiSymbolViewParams poiParams(m_id);
    m2::PointD shieldPixelSize;
    GetRoadShieldsViewParams(texMng, shield, shieldIndex, static_cast<uint8_t>(roadShields.size()),
                             textParams, symbolParams, poiParams, shieldPixelSize);

    auto & generatedShieldRects = generatedRoadShields[shield];
    generatedShieldRects.reserve(10);
    for (auto const & shieldPos : shieldPositions)
    {
      if (!CheckShieldsNearby(shieldPos, shieldPixelSize, scaledMinDistance, generatedShieldRects))
        continue;

      m_insertShape(make_unique_dp<TextShape>(shieldPos, textParams, m_tileKey, true /* hasPOI */,
                                              m2::PointF(0.0f, 0.0f) /* symbolSize */, textIndex));
      if (IsColoredRoadShield(shield))
        m_insertShape(make_unique_dp<ColoredSymbolShape>(shieldPos, symbolParams, m_tileKey, textIndex));
      else if (IsSymbolRoadShield(shield))
        m_insertShape(make_unique_dp<PoiSymbolShape>(shieldPos, poiParams, m_tileKey, textIndex));
      textIndex++;
    }
    shieldIndex++;
  }
}
}  // namespace df
