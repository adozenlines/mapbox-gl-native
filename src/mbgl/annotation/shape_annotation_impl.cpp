#include <mapbox/geojsonvt/geojsonvt_convert.hpp>

#include <mbgl/annotation/shape_annotation_impl.hpp>
#include <mbgl/annotation/annotation_manager.hpp>
#include <mbgl/annotation/annotation_tile.hpp>
#include <mbgl/util/constants.hpp>
#include <mbgl/util/string.hpp>
#include <mbgl/style/style.hpp>
#include <mbgl/layer/line_layer.hpp>
#include <mbgl/layer/fill_layer.hpp>

namespace mbgl {

using namespace mapbox::util::geojsonvt;

ShapeAnnotationImpl::ShapeAnnotationImpl(const AnnotationID id_,
                                         const ShapeAnnotation& shape_,
                                         const uint8_t maxZoom_)
: id(id_),
  layerID("com.mapbox.annotations.shape." + util::toString(id)),
  shape(shape_),
  maxZoom(maxZoom_) {
}

void ShapeAnnotationImpl::updateStyle(Style& style) {
    if (style.getLayer(layerID))
        return;

    if (shape.properties.is<LineAnnotationProperties>()) {
        type = ProjectedFeatureType::LineString;

        std::unique_ptr<LineLayer> layer = std::make_unique<LineLayer>();
        layer->type = StyleLayerType::Line;
        layer->layout.join = JoinType::Round;

        const LineAnnotationProperties& properties = shape.properties.get<LineAnnotationProperties>();
        layer->paint.opacity = properties.opacity;
        layer->paint.width = properties.width;
        layer->paint.color = properties.color;

        layer->id = layerID;
        layer->source = AnnotationManager::SourceID;
        layer->sourceLayer = layer->id;

        style.addLayer(std::move(layer), AnnotationManager::PointLayerID);

    } else if (shape.properties.is<FillAnnotationProperties>()) {
        type = ProjectedFeatureType::Polygon;

        std::unique_ptr<FillLayer> layer = std::make_unique<FillLayer>();
        layer->type = StyleLayerType::Fill;

        const FillAnnotationProperties& properties = shape.properties.get<FillAnnotationProperties>();
        layer->paint.opacity = properties.opacity;
        layer->paint.color = properties.color;
        layer->paint.outlineColor = properties.outlineColor;

        layer->id = layerID;
        layer->source = AnnotationManager::SourceID;
        layer->sourceLayer = layer->id;

        style.addLayer(std::move(layer), AnnotationManager::PointLayerID);

    } else {
        const StyleLayer* sourceLayer = style.getLayer(shape.properties.get<std::string>());
        if (!sourceLayer) return;

        std::unique_ptr<StyleLayer> layer = sourceLayer->clone();

        type = layer->type == StyleLayerType::Line
            ? ProjectedFeatureType::LineString
            : ProjectedFeatureType::Polygon;

        layer->id = layerID;
        layer->source = AnnotationManager::SourceID;
        layer->sourceLayer = layer->id;

        style.addLayer(std::move(layer), sourceLayer->id);
    }
}

void ShapeAnnotationImpl::updateTile(const TileID& tileID, AnnotationTile& tile) {
    static const double baseTolerance = 3;
    static const uint16_t extent = 4096;

    if (!shapeTiler) {
        const uint64_t maxAmountOfTiles = 1 << maxZoom;
        const double tolerance = baseTolerance / (maxAmountOfTiles * extent);

        ProjectedGeometryContainer rings;
        std::vector<LonLat> points;

        for (size_t i = 0; i < shape.segments[0].size(); ++i) { // first segment for now (no holes)
            const double constraintedLatitude = ::fmin(::fmax(shape.segments[0][i].latitude, -util::LATITUDE_MAX), util::LATITUDE_MAX);
            points.push_back(LonLat(shape.segments[0][i].longitude, constraintedLatitude));
        }

        if (type == ProjectedFeatureType::Polygon &&
                (points.front().lon != points.back().lon || points.front().lat != points.back().lat)) {
            points.push_back(LonLat(points.front().lon, points.front().lat));
        }

        ProjectedGeometryContainer ring = Convert::project(points, tolerance);
        rings.members.push_back(ring);

        std::vector<ProjectedFeature> features;
        features.push_back(Convert::create(Tags(), type, rings));

        shapeTiler = std::make_unique<mapbox::util::geojsonvt::GeoJSONVT>(features, maxZoom, 4, 100, 10);
    }

    const auto& shapeTile = shapeTiler->getTile(tileID.z, tileID.x, tileID.y);
    if (!shapeTile)
        return;

    AnnotationTileLayer& layer = *tile.layers.emplace(layerID,
        std::make_unique<AnnotationTileLayer>()).first->second;

    for (auto& shapeFeature : shapeTile.features) {
        FeatureType featureType = FeatureType::Unknown;

        if (shapeFeature.type == TileFeatureType::LineString) {
            featureType = FeatureType::LineString;
        } else if (shapeFeature.type == TileFeatureType::Polygon) {
            featureType = FeatureType::Polygon;
        }

        assert(featureType != FeatureType::Unknown);

        GeometryCollection renderGeometry;
        for (auto& shapeGeometry : shapeFeature.tileGeometry) {
            std::vector<Coordinate> renderLine;
            auto& shapeRing = shapeGeometry.get<TileRing>();

            for (auto& shapePoint : shapeRing.points) {
                renderLine.emplace_back(shapePoint.x, shapePoint.y);
            }

            renderGeometry.push_back(renderLine);
        }

        layer.features.emplace_back(
            std::make_shared<AnnotationTileFeature>(featureType, renderGeometry));
    }
}

LatLngBounds ShapeAnnotationImpl::bounds() const {
    LatLngBounds result;

    for (const auto& segment : shape.segments) {
        for (const auto& point : segment) {
            result.extend(point);
        }
    }

    return result;
}

}
