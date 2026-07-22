/**
 * Parley — request/response RPC over MQTT (TypeScript/JS reference implementation of protocol v1).
 *
 * Bring your own MQTT client via a {@link ParleyConnectionAdapter}; the core is transport-free and
 * has zero runtime dependencies. Expected outcomes resolve as {@link ParleyResult} values — never
 * rejections.
 */

export { ParleyConnectionAdapter, type ParleyMessageHandler } from './adapter';
export {
  ParleyCommander,
  type ParleyCommanderCodec,
  type ParleyCommanderOptions,
  type ParleyExecuteOptions,
} from './commander';
export {
  ParleyCommandHandler,
  type ParleyHandlerCodec,
  type ParleyHandlerOptions,
} from './handler';
export {
  ParleyTopics,
  type ParleyCallOptions,
  type ParleyConfig,
} from './config';
export {
  ParleyBuiltinError,
  ParleyErrorCodes,
  ParleyOutcome,
  toBuiltin,
  type ParleyBuiltinErrorCode,
  type ParleyError,
  type ParleyOutcomeKind,
  type ParleyResult,
} from './errors';
export {
  type ParleyAbortSignal,
  type ParleyExecution,
} from './execution';
export {
  SystemClock,
  type ParleyClock,
  type ParleyTimerHandle,
} from './clock';
export {
  DefaultIdGenerator,
  type ParleyIdGenerator,
} from './idGenerator';
