/**
 * Standard API Compliance Module
 * Industry best practices for API usage:
 * - Authentication & token validation
 * - Rate limiting / throttling
 * - Input validation
 * - Error handling & logging
 * - CORS & security headers
 */

const API_BASE = window.__API_BASE__ || 'https://medorcoin.org';

/**
 * Validate input parameters to prevent injection or malformed requests
 * @param {Object} params - API parameters
 * @returns {boolean} true if valid
 */
function validateInput(params) {
  try {
    for (const key in params) {
      if (typeof params[key] === 'string') {
        if (/[\<\>\'\"\;]/.test(params[key])) {
          console.warn(`Suspicious input detected: ${key} = ${params[key]}`);
          return false;
        }
      }
    }
    return true;
  } catch (err) {
    console.error('Input validation error', err);
    return false;
  }
}

/**
 * Retrieve auth token safely from localStorage or cookie
 * @returns {string|null} token
 */
function getAuthToken() {
  const token = localStorage.getItem('token');
  if (token) return token;

  const match = document.cookie.match(/(?:^|;\s*)token=([^;]+)/);
  return match ? decodeURIComponent(match[1]) : null;
}

/**
 * Standardized fetch wrapper with compliance features
 * @param {string} endpoint
 * @param {Object} options
 * @param {number} timeoutMs
 * @returns {Promise<any>}
 */
async function compliantFetch(endpoint, options = {}, timeoutMs = 8000) {
  const token = getAuthToken();
  if (!token) throw new Error('Unauthorized: No auth token found.');

  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), timeoutMs);

  try {
    const response = await fetch(`${API_BASE}${endpoint}`, {
      ...options,
      headers: {
        'Authorization': `Bearer ${token}`,
        'Content-Type': 'application/json',
        ...(options.headers || {}),
      },
      signal: controller.signal,
      credentials: 'include',
    });

    clearTimeout(timeout);

    if (response.status === 401 || response.status === 403) {
      console.warn('Authentication failed. Clearing session.');
      localStorage.removeItem('token');
      document.cookie = 'token=; expires=Thu, 01 Jan 1970 00:00:00 UTC; path=/;';
      throw new Error('Unauthorized or session expired.');
    }

    if (!response.ok) {
      const err = await response.json().catch(() => ({}));
      const message = err.error || response.statusText || 'Unknown error';
      throw new Error(`API Error: ${message}`);
    }

    return await response.json();
  } catch (err) {
    if (err.name === 'AbortError') throw new Error('Request timed out.');
    throw err;
  }
}

/**
 * Rate limiter: basic token bucket implementation
 * @param {number} limit - number of allowed calls
 * @param {number} intervalMs - time window
 */
function rateLimiter(limit = 5, intervalMs = 60000) {
  let calls = 0;
  const queue = [];

  function schedule(fn) {
    if (calls < limit) {
      calls++;
      fn();
      setTimeout(() => {
        calls--;
        if (queue.length > 0) schedule(queue.shift());
      }, intervalMs);
    } else {
      queue.push(fn);
    }
  }

  return schedule;
}

/**
 * Logging wrapper
 * @param {string} action
 * @param {any} details
 */
function logCompliance(action, details) {
  console.info(`[COMPLIANCE] ${action}`, details || {});
}

// Export for other modules
window.apiCompliance = {
  validateInput,
  getAuthToken,
  compliantFetch,
  rateLimiter,
  logCompliance
};
