import { isReadOnlyBatch, isReadOnlyMotorStatus } from './read-only-device-requests.mjs';

// Only classify a health cancellation when the application itself reported
// page cancellation and its governor generation advanced during that call.
export function isIntentionalHealthCancellation(failure, evidence) {
  return failure.route === '/api/health' && failure.method === 'GET' &&
    failure.reason === 'net::ERR_ABORTED' && evidence.some(item =>
      item.route === failure.route && item.pageAborted === true && item.requestTimeout !== true &&
      Number.isInteger(item.startGeneration) && Number.isInteger(item.endGeneration) &&
      item.endGeneration > item.startGeneration &&
      failure.at >= item.startedAt && failure.at <= item.finishedAt + 1000);
}

// The governor aborts in-flight requests on page navigation and also aborts
// them on its 15s request timeout; both surface as net::ERR_ABORTED. Exempt an
// abort only when the request is an idempotent read, is younger than the
// governor timeout (so genuine stalls still fail), and was interrupted by a
// menu switch: either it predates the latest navigation generation, or it
// belongs to the interrupted generation inside the bounded navigation window.
const GOVERNOR_TIMEOUT_ABORT_EXCLUSION_MS = 14000;
export function isNavigatedAbortedRead(request, reason, requestGeneration, generation, navigation, now, startedAt) {
  if (reason !== 'net::ERR_ABORTED' || !Number.isInteger(requestGeneration) || !Number.isInteger(generation) ||
      !Number.isFinite(startedAt) || now - startedAt >= GOVERNOR_TIMEOUT_ABORT_EXCLUSION_MS) return false;
  const interruptedByNavigation = requestGeneration < generation;
  const inNavigationWindow = !!navigation && Number.isInteger(navigation.generation) &&
    requestGeneration <= navigation.generation && now >= navigation.at && now <= navigation.until;
  if (!interruptedByNavigation && !inNavigationWindow) return false;
  let target;
  try { target = new URL(request.url()); } catch { return false; }
  // Health aborts stay on the governor-evidence path of isIntentionalHealthCancellation.
  if (target.pathname === '/api/health') return false;
  if (['GET', 'HEAD', 'OPTIONS'].includes(request.method())) return true;
  const contentType = request.headers()['content-type'];
  return isReadOnlyBatch(request.method(), request.url(), request.postData(), contentType) ||
    isReadOnlyMotorStatus(request.method(), request.url(), request.postData(), contentType);
}
