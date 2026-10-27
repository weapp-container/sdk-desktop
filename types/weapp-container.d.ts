export type JsonValue = null | boolean | number | string | JsonValue[] | { [key: string]: JsonValue };
export interface HostInvokeOptions { timeout?: number; cancelToken?: string; }
export interface WeappContainerBridge {
  invoke(method: string, payload?: JsonValue, options?: HostInvokeOptions): Promise<JsonValue>;
  postMessage(payload: JsonValue): void;
  onMessage(listener: (payload: JsonValue) => void): void;
  offMessage(listener?: (payload: JsonValue) => void): void;
}
declare global { const weappContainer: WeappContainerBridge; }
