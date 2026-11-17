export type JsonValue = null | boolean | number | string | JsonValue[] | { [key: string]: JsonValue };
export interface HostInvokeOptions { timeout?: number; cancelToken?: string; }
export interface WeappContainerBridge {
  invoke(method: string, payload?: JsonValue, options?: HostInvokeOptions): Promise<JsonValue>;
  postMessage(payload: JsonValue): void;
  onMessage(listener: (payload: JsonValue) => void): void;
  offMessage(listener?: (payload: JsonValue) => void): void;
}
declare global { const weappContainer: WeappContainerBridge; }

/** Additive SDK chrome contract, capability chrome.protocolVersion === "1.0". */
export interface ChromePage { id: number; route: string; }
export interface HostChromeGeometry {
  schemaVersion: 1;
  revision: number;
  /** Rectangles use actual current-screen logical pixels. */
  contentRect: { left: number; top: number; width: number; height: number };
  menuButtonRect: { left: number; right: number; top: number; bottom: number; width: number; height: number };
  sessionId?: string;
  pageId?: number;
}
export interface PageChrome {
  schemaVersion: 1;
  chromeRevision: number;
  sessionId: string | null;
  page: ChromePage | null;
  stack: ChromePage[];
  canGoBack: boolean;
  navigationStyle: "default" | "custom";
  title: string;
  backgroundColor: string;
  frontColor: "#000000" | "#ffffff";
  tabBar: {
    visible: boolean;
    custom: boolean;
    color: string;
    selectedColor: string;
    backgroundColor: string;
    borderStyle: string;
    selectedIndex: number | null;
    /** Dynamic indicators require matching runtime admission and an attached system presenter. */
    list: Array<{ pagePath: string; text: string; iconPath?: string; selectedIconPath?: string; badgeText?: string; redDot?: boolean }>;
  };
}
export interface ChromeCapabilities {
  protocolVersion: "1.0";
  geometry: true;
  inspectPack: true;
  occlusion: true;
  transactionalRestart: true;
}
