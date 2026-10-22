'use client';

import { useEffect, useRef, useState } from 'react';
import maplibregl, {
  type ExpressionSpecification,
  type LngLatBoundsLike,
  type MapGeoJSONFeature,
  type MapMouseEvent,
  type StyleSpecification,
  type VectorTileSource,
} from 'maplibre-gl';
import { baseTileUrl, deg, HIGHWAY_CLASS, userTileUrl, type TypeIdx, type WRegion } from '../api';

export type FlyTarget =
  | { kind: 'bbox'; minLat: number; minLon: number; maxLat: number; maxLon: number; seq: number }
  | { kind: 'point'; lat: number; lon: number; zoom: number; seq: number };

interface Props {
  userId: number | null;
  type: TypeIdx;
  coverageVersion: number;
  regions: WRegion[] | null;
  fly: FlyTarget | null;
  onHome: () => void;
  connecting: boolean;
}

type Layer = 'traveled' | 'untraveled' | 'unique';
const LAYERS: readonly Layer[] = ['unique', 'untraveled', 'traveled'];
const COLOR: Record<Layer, string> = { traveled: '#2f6fe4', untraveled: '#e0453a', unique: '#8b3fc9' };
const USER_LAYER: Record<Layer, string> = { traveled: 'user-traveled', untraveled: 'user-untraveled', unique: 'user-unique' };

// A road's width at z14 by its class (tag "c", ADV_HC_*), scaled by zoom around that.
const CLASS_WIDTH: ExpressionSpecification = ['match', ['get', 'c'], [0, 1, 2], 3, [3, 4], 2, [5, 6], 1.4, 0.8];
const ROAD_WIDTH: ExpressionSpecification = [
  'interpolate', ['exponential', 1.6], ['zoom'],
  8, ['*', CLASS_WIDTH, 0.15],
  11, ['*', CLASS_WIDTH, 0.4],
  14, CLASS_WIDTH,
  18, ['*', CLASS_WIDTH, 4],
];

function scaled(width: number): ExpressionSpecification {
  return ['interpolate', ['exponential', 1.5], ['zoom'], 9, width * 0.35, 12, width * 0.6, 14, width, 18, width * 2.5];
}

// An empty tile template for "no user yet": a URL that never resolves to a tile would 404 on
// every move, so the source starts hidden and gets its template when a user is selected.
const BLANK_USER = `${baseTileUrl()}?none`;

function style(): StyleSpecification {
  return {
    version: 8,
    sources: {
      base: { type: 'vector', tiles: [baseTileUrl()], minzoom: 0, maxzoom: 16 },
      user: { type: 'vector', tiles: [BLANK_USER], minzoom: 0, maxzoom: 16 },
    },
    layers: [
      { id: 'bg', type: 'background', paint: { 'background-color': '#f4f1ea' } },
      {
        id: 'roads',
        type: 'line',
        source: 'base',
        'source-layer': 'roads',
        layout: { 'line-cap': 'round', 'line-join': 'round' },
        paint: { 'line-color': '#c9c3b5', 'line-width': ROAD_WIDTH },
      },
      {
        id: USER_LAYER.unique,
        type: 'line',
        source: 'user',
        'source-layer': 'unique',
        layout: { 'line-cap': 'round', 'line-join': 'round', visibility: 'none' },
        paint: { 'line-color': COLOR.unique, 'line-width': scaled(4), 'line-opacity': 0.9 },
      },
      {
        id: USER_LAYER.untraveled,
        type: 'line',
        source: 'user',
        'source-layer': 'untraveled',
        layout: { 'line-cap': 'round', 'line-join': 'round', visibility: 'none' },
        paint: { 'line-color': COLOR.untraveled, 'line-width': scaled(2.2), 'line-opacity': 0.6 },
      },
      {
        id: USER_LAYER.traveled,
        type: 'line',
        source: 'user',
        'source-layer': 'traveled',
        layout: { 'line-cap': 'round', 'line-join': 'round', visibility: 'none' },
        paint: { 'line-color': COLOR.traveled, 'line-width': scaled(3) },
      },
    ],
  };
}

function regionsBounds(regions: WRegion[]): LngLatBoundsLike | null {
  let minLat = Infinity, minLon = Infinity, maxLat = -Infinity, maxLon = -Infinity;
  for (const r of regions) {
    if (r.minLat < minLat) minLat = r.minLat;
    if (r.minLon < minLon) minLon = r.minLon;
    if (r.maxLat > maxLat) maxLat = r.maxLat;
    if (r.maxLon > maxLon) maxLon = r.maxLon;
  }
  if (!isFinite(minLat) || minLat > maxLat) return null;
  return [[deg(minLon), deg(minLat)], [deg(maxLon), deg(maxLat)]];
}

export function MapView({ userId, type, coverageVersion, regions, fly, onHome, connecting }: Props) {
  const container = useRef<HTMLDivElement>(null);
  const mapRef = useRef<maplibregl.Map | null>(null);
  const [ready, setReady] = useState(false);
  const [visible, setVisible] = useState<Record<Layer, boolean>>({ traveled: true, untraveled: true, unique: true });
  const fitted = useRef(false);

  useEffect(() => {
    if (!container.current || mapRef.current) return;
    const map = new maplibregl.Map({
      container: container.current,
      style: style(),
      center: [0, 20],
      zoom: 1.5,
      attributionControl: false,
      maxZoom: 19,
    });
    map.addControl(new maplibregl.NavigationControl({ showCompass: false }), 'top-right');
    map.addControl(
      new maplibregl.AttributionControl({ compact: false, customAttribution: '© OpenStreetMap contributors' }),
      'bottom-right',
    );

    const popup = new maplibregl.Popup({ closeButton: false, closeOnClick: false, offset: 8 });
    const hoverable = [USER_LAYER.traveled, USER_LAYER.untraveled];
    const onMove = (e: MapMouseEvent & { features?: MapGeoJSONFeature[] }) => {
      const f = e.features && e.features[0];
      if (!f) return;
      const c = Number(f.properties?.c);
      const which = f.layer.id === USER_LAYER.traveled ? 'traveled' : 'untraveled';
      map.getCanvas().style.cursor = 'pointer';
      popup.setLngLat(e.lngLat).setHTML(`<b>${HIGHWAY_CLASS[c] ?? 'road'}</b> <span style="color:#6b7280">${which}</span>`).addTo(map);
    };
    const onLeave = () => {
      map.getCanvas().style.cursor = '';
      popup.remove();
    };
    for (const id of hoverable) {
      map.on('mousemove', id, onMove);
      map.on('mouseleave', id, onLeave);
    }
    // Tiles that 404 while the server warms up are not worth a console per tile.
    map.on('error', () => {});
    map.on('load', () => setReady(true));
    mapRef.current = map;
    return () => {
      popup.remove();
      map.remove();
      mapRef.current = null;
      setReady(false);
    };
  }, []);

  // The user source's template: user, type tab and coverage version together.
  useEffect(() => {
    const map = mapRef.current;
    if (!map || !ready) return;
    const src = map.getSource('user') as VectorTileSource | undefined;
    if (!src) return;
    if (userId === null) {
      src.setTiles([BLANK_USER]);
      for (const l of LAYERS) map.setLayoutProperty(USER_LAYER[l], 'visibility', 'none');
      return;
    }
    src.setTiles([userTileUrl(userId, type, coverageVersion)]);
    for (const l of LAYERS) map.setLayoutProperty(USER_LAYER[l], 'visibility', visible[l] ? 'visible' : 'none');
  }, [ready, userId, type, coverageVersion, visible]);

  // The first view: the regions' bbox, once.
  useEffect(() => {
    const map = mapRef.current;
    if (!map || !ready || fitted.current || !regions || regions.length === 0) return;
    const b = regionsBounds(regions);
    if (!b) return;
    fitted.current = true;
    map.fitBounds(b, { padding: 24, duration: 0 });
  }, [ready, regions]);

  useEffect(() => {
    const map = mapRef.current;
    if (!map || !ready || !fly) return;
    if (fly.kind === 'bbox') {
      map.fitBounds([[deg(fly.minLon), deg(fly.minLat)], [deg(fly.maxLon), deg(fly.maxLat)]], { padding: 32, duration: 900 });
    } else {
      map.flyTo({ center: [deg(fly.lon), deg(fly.lat)], zoom: fly.zoom, duration: 1200 });
    }
  }, [ready, fly]);

  return (
    <section className="card map-card">
      <div className="map" ref={container} />
      <div className="map-ui">
        <div className="map-panel">
          {(['traveled', 'untraveled', 'unique'] as Layer[]).map((l) => (
            <label key={l}>
              <input
                type="checkbox"
                checked={visible[l]}
                onChange={(e) => setVisible((v) => ({ ...v, [l]: e.target.checked }))}
              />
              <span className="swatch" style={{ background: COLOR[l] }} />
              {l}
            </label>
          ))}
        </div>
        <button type="button" className="btn" onClick={onHome} disabled={userId === null}>
          Home
        </button>
      </div>
      {connecting && <div className="map-status">connecting to the server...</div>}
    </section>
  );
}
