const LABELS = {
  google: "Continue with Google",
  apple: "Continue with Apple",
  microsoft: "Continue with Microsoft",
};

const ORDER = ["google", "apple", "microsoft"];

export async function renderProviderButtons(container, { intent } = {}) {
  const res = await fetch("/api/auth/providers");
  const data = await res.json();
  const providers = data.providers || {};
  container.innerHTML = "";

  let any = false;
  for (const id of ORDER) {
    if (!providers[id]) continue;
    any = true;
    const a = document.createElement("a");
    a.className = `auth-btn ${id === "microsoft" ? "microsoft" : "secondary"}`;
    a.textContent = LABELS[id];
    const q = intent ? `?intent=${encodeURIComponent(intent)}` : "";
    a.href = `/auth/${id}${q}`;
    container.appendChild(a);
  }

  if (!any) {
    const p = document.createElement("p");
    p.className = "auth-error";
    p.hidden = false;
    p.textContent =
      "No sign-in providers configured. Set Microsoft and/or Google (and optionally Apple) in server .env.";
    container.appendChild(p);
  }
}
