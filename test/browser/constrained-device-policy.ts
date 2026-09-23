/**
 * Load-shedding policy for real devices with limited DRAM or weak Wi-Fi.
 *
 * Playwright replaces a worker after a failed test. Because the authentication
 * cache is intentionally memory-only, allowing that replacement to continue
 * would trigger another full login (including PBKDF2) for every new worker.
 * Stop on the first visible failure instead of turning one regression into a
 * stream of authentication and static-resource requests.
 */
process.env.FASTBEE_CONSTRAINED_DEVICE = '1';

export const constrainedDevicePolicy = {
  fullyParallel: false,
  workers: 1,
  retries: 0,
  maxFailures: 1,
} as const;
