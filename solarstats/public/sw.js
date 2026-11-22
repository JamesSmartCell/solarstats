// Service Worker for Web Push Notifications

self.addEventListener("install", (event) => {
  console.log("[SW] Install");
  self.skipWaiting();
});

self.addEventListener("activate", (event) => {
  console.log("[SW] Activate");
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
  console.log("[SW] Notification click");
  event.notification.close();
  
  const url = event.notification.data?.url || "/";
  
  event.waitUntil(
    self.clients.matchAll({ type: "window", includeUncontrolled: true }).then((clientList) => {
      // Try to focus an existing window
      for (const client of clientList) {
        if (client.url === url && "focus" in client) {
          return client.focus();
        }
      }
      
      // Open a new window if no matching window found
      if (self.clients.openWindow) {
        return self.clients.openWindow(url);
      }
    })
  );
});
