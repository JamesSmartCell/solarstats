// Service Worker for Web Push Notifications v2

self.addEventListener("install", (event) => {
  console.log("[SW] Install v2");
  self.skipWaiting();
});

self.addEventListener("activate", (event) => {
  console.log("[SW] Activate v2");
  event.waitUntil(self.clients.claim());
});

self.addEventListener("push", (event) => {
  console.log("[SW] Push received");
  
  if (!event.data) {
    console.warn("[SW] Push event has no data");
    return;
  }
  
  try {
    const data = event.data.json();
    console.log("[SW] Push data:", data);
    
    const options = {
      body: data.body || "",
      icon: data.icon || "/android-chrome-192x192.png",
      badge: data.badge || "/favicon-32x32.png",
      tag: data.tag || "alert",
      renotify: data.renotify !== false,
      vibrate: data.vibrate || [200, 100, 200],
      data: {
        url: data.url || "/"
      }
    };
    
    event.waitUntil(
      self.registration.showNotification(data.title || "Alert", options)
    );
  } catch (err) {
    console.error("[SW] Failed to show notification:", err);
  }
});

self.addEventListener("notificationclick", (event) => {
  console.log("[SW] Notification click v2");
  event.notification.close();
  
  const relativeUrl = event.notification.data?.url || "/";
  
  // Resolve URL against service worker scope to get absolute URL
  const targetUrl = new URL(relativeUrl, self.registration.scope).href;
  const targetOrigin = new URL(targetUrl).origin;
  
  event.waitUntil(
    self.clients.matchAll({ type: "window", includeUncontrolled: true }).then(async (clientList) => {
      // Try to find an existing tab on the same origin
      for (const client of clientList) {
        try {
          const clientOrigin = new URL(client.url).origin;
          if (clientOrigin === targetOrigin && "focus" in client) {
            // Focus the existing tab and navigate it to the target URL
            const focusedClient = await client.focus();
            if ("navigate" in focusedClient) {
              await focusedClient.navigate(targetUrl);
            }
            return focusedClient;
          }
        } catch (err) {
          console.warn("[SW] Error checking client:", err);
        }
      }
      
      // Open a new window if no matching window found
      if (self.clients.openWindow) {
        return self.clients.openWindow(targetUrl);
      }
    })
  );
});
