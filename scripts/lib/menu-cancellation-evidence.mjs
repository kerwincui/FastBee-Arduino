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
