<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8"/>
  <meta name="viewport" content="width=device-width,initial-scale=1"/>
  <title>MedorCoin — Home</title>
  <meta name="description" content="MedorCoin home portal."/>
  <style>
    :root{--bg:#0f172a;--ink:#e5e7eb;--muted:#94a3b8;--border:#334155;--success:#10b981}
    *{box-sizing:border-box}
    body{margin:0;font-family:Inter,Segoe UI,Roboto,Helvetica,Arial,sans-serif;background:var(--bg);color:#e2e8f0}
    a{color:inherit;text-decoration:none}
    .container{max-width:980px;margin:0 auto;padding:12px}
    .row{display:flex;align-items:center;gap:10px;flex-wrap:wrap}
    .space{justify-content:space-between}
    .logo{display:flex;align-items:center;gap:10px}
    .logo img{height:36px;width:36px;border-radius:10px;object-fit:cover;border:1px solid #334155}
    .brand{font-weight:800;font-size:18px}
    /* FIX 6: wallet status badge */
    .wallet-status{
      position:fixed;top:10px;right:10px;
      background:var(--success);color:white;
      padding:8px 14px;border-radius:20px;
      font-size:12px;font-weight:600;z-index:1000;
      box-shadow:0 4px 12px rgba(16,185,129,0.3);
      display:none;cursor:pointer;
    }
    .searchbar{display:flex;align-items:center;gap:8px;background:#111827;border:1px solid var(--border);border-radius:999px;padding:10px 12px;margin-top:12px}
    .searchbar input{border:0;outline:0;flex:1;font-size:14px;color:#e5e7eb;background:transparent}
    .pill{font-size:12px;background:#1e293b;color:#e5e7eb;border:1px solid #334155;border-radius:999px;padding:4px 8px}
    .tabs{display:flex;gap:10px;overflow:auto;padding:10px 0;margin-top:8px}
    .tab{display:flex;align-items:center;gap:6px;background:#1f2937;border:1px solid var(--border);color:#e2e8f0;border-radius:999px;padding:8px 12px;font-size:13px;white-space:nowrap}
    .grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(140px,1fr));gap:10px;margin-top:12px}
    .tile{background:#fff;border:1px solid var(--border);border-radius:14px;padding:12px;min-height:96px;display:flex;flex-direction:column;gap:8px;justify-content:center}
    .muted{color:var(--muted);font-size:12px}
    .icon{width:34px;height:34px;border-radius:10px;display:flex;align-items:center;justify-content:center}
    .icon svg{width:22px;height:22px;display:block}
    .bg-blue{background:#0B3D91;color:#fff;border:1px solid var(--border)}
    .bg-teal{background:#87CEEB;color:#333;border:1px solid var(--border)}
    .bg-orange{background:#FFD6A3;color:#333;border:1px solid var(--border)}
    .bg-dark{background:#3F3F3F;color:#FDE68A;border:1px solid var(--border)}
  </style>
</head>
<body>

<!-- FIX 6: wallet status — shows address if wallet found -->
<div id="walletStatus" class="wallet-status" onclick="window.location.href='wallet.html'"></div>

<div class="container">

  <!-- HEADER -->
  <div class="row space" style="background:linear-gradient(135deg,#1f3a8a 0%,#1e40af 100%);color:white;padding:12px 16px;border-radius:14px;margin-bottom:14px;align-items:center;">
    <div class="logo" style="display:flex;align-items:center;gap:12px;">
      <img src="assets/logo.svg" alt="MedorCoin Logo" height="36" style="width:36px;height:36px;border-radius:10px;object-fit:cover;border:1px solid #e8e1c7;">
      <span class="brand" style="font-weight:800;font-size:18px;">MedorCoin</span>
    </div>
    <div style="font-size:12px;display:flex;gap:12px;align-items:center;">
      <a href="create-wallet.html" style="color:white;">Create Wallet</a> |
      <a href="connect-wallet.html" style="color:white;">Connect Wallet</a> |
      <a href="signup.html" style="color:white;">SignUp</a> |
      <a href="login.html" style="color:white;">Login</a>
    </div>
  </div>

  <!-- FIX 4: search with basic filter -->
  <div class="searchbar">
    <span style="opacity:.7">🔎</span>
    <input id="q" placeholder="Search tiles..." oninput="filterTiles(this.value)"/>
    <span class="pill">AI</span>
  </div>

  <div class="tabs">
    <a class="tab" href="index.html">🏠 HomePage</a>
    <a class="tab" href="miners.html">⛏️ Miners</a>
    <a class="tab" href="developers.html">👩‍💻 Developers</a>
    <a class="tab" href="trading.html">📈 Crypto Trading</a>
  </div>

  <div id="welcomeNote" style="text-align:center;margin:6px 0 14px;">
    <span style="color:#EDE3DA;font-weight:700;font-size:16px;">
      Welcome to MedorCoin — explore tools, APIs, and insights.
    </span>
  </div>

  <div id="ticker" style="text-align:center;margin:6px 0 14px;">
    <script type="module" src="https://widgets.tradingview-widget.com/w/en/tv-ticker-tape.js"></script>
    <tv-ticker-tape symbols='BINANCE:BTCUSDT,BINANCE:ETHUSDT,BINANCE:XRPUSDT,BINANCE:BNBUSDT,BINANCE:LINKUSDT,BINANCE:SOLUSDT'></tv-ticker-tape>
  </div>

  <div class="grid" id="tileGrid">

    <a class="tile bg-teal" href="/blockchain.html">
      <div class="icon"><svg viewBox="0 0 24 24" fill="none" stroke="#B5943F" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"><circle cx="5" cy="6" r="2.4" fill="#F1E8C6" stroke="#B5943F"/><circle cx="19" cy="6" r="2.4" fill="#F1E8C6" stroke="#B5943F"/><circle cx="5" cy="18" r="2.4" fill="#F1E8C6" stroke="#B5943F"/><circle cx="19" cy="18" r="2.4" fill="#F1E8C6" stroke="#B5943F"/><path d="M7.2 6h9.6"/><path d="M5 8.6v6.8"/><path d="M19 8.6v6.8"/></svg></div>
      <b>Blockchain</b>
      <span class="muted">On-chain tools & explorer</span>
    </a>

    <a class="tile" href="combined_api_portal.html" style="background:#87CEEB;color:#1f1f1f;border:1px solid var(--border);">
      <div class="icon" style="font-size:20px;">🔗</div>
      <b>Rent/Buy API</b>
      <span class="muted" style="color:#1f1f1f;">API access</span>
    </a>

    <a class="tile" href="miners.html" style="background:#FFD6A3;color:#333;border:1px solid var(--border);">
      <div class="icon">⛏️</div>
      <b>For Miners</b>
      <span class="muted">Start mining</span>
    </a>

    <a class="tile" href="developers.html" style="background:#87CEEB;color:#333;border:1px solid var(--border);">
      <div class="icon">👩‍💻</div>
      <b>For Developers</b>
      <span class="muted">Docs & SDKs</span>
    </a>

    <a class="tile" href="crypto-trading.html" style="background:#2f2f2f;color:#FDE68A;border:1px solid var(--border);">
      <div class="icon"><svg viewBox="0 0 24 24" fill="none" stroke="#FDE68A" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"><path d="M3 12h4l3-7 4 14 5-9"/></svg></div>
      <b style="color:#FDE68A;">Crypto Trading</b>
      <span class="muted" style="color:#FDE68A;">Markets & tools</span>
    </a>

    <a class="tile" href="learn.html" style="background:#0B3D91;color:#fff;border:1px solid var(--border);">
      <div class="icon">📚</div>
      <b>Courses</b>
      <span class="muted" style="color:#fff;">Guides</span>
    </a>

    <a class="tile" href="Chatroom.html" style="background:#E6D5FF;color:#A089FF;border:1px solid var(--border);">
      <div class="icon">💬</div>
      <b>Chatroom</b>
      <span class="muted" style="color:#A089FF;">Chat & support</span>
    </a>

    <a class="tile" href="crypto-whales.html" style="background:#F5F5DC;color:#333;border:1px solid var(--border);">
      <div class="icon">🐋</div>
      <b>Crypto Whales</b>
      <span class="muted" style="color:#333;">Large flows</span>
    </a>

    <!-- FIX 5: removed invalid border:1px solid none(--border) -->
    <a class="tile" href="more.html" style="background:#1e293b;color:#fff;border:1px solid var(--border);">
      <div class="icon" style="font-size:18px;">＋</div>
      <b>More</b>
      <span class="muted" style="color:#94a3b8;">All features</span>
    </a>

  </div>

  <!-- TradingView Screener -->
  <div class="tradingview-widget-container" style="margin:20px 0;">
    <div class="tradingview-widget-container__widget"></div>
    <div class="tradingview-widget-copyright">
      <a href="https://www.tradingview.com/markets/cryptocurrencies/prices-all/" rel="noopener nofollow" target="_blank">
        <span class="blue-text">Crypto markets</span>
      </a> by TradingView
    </div>
    <script type="text/javascript" src="https://s3.tradingview.com/external-embedding/embed-widget-screener.js" async>
    {
      "defaultColumn": "overview",
      "screener_type": "crypto_mkt",
      "displayCurrency": "USD",
      "colorTheme": "dark",
      "isTransparent": false,
      "locale": "en",
      "width": "100%",
      "height": 550
    }
    </script>
  </div>

  <!-- FIX 1+2: closed section and container tags properly -->
  <section style="padding:20px 12px;">
    <div style="display:grid;grid-template-columns:1fr 1fr;gap:28px;">
      <div>
        <h3>General</h3>
        <ul style="padding-left:20px;margin:6px 0 0;">
          <li><a href="beware-of-scams.html">Beware of Scams</a></li>
          <li><a href="buy-sell-medorcoin.html">Buy & Sell</a></li>
          <li><a href="invisible-m-blog.html">Blog</a></li>
          <li><a href="legal.html">Legal</a></li>
          <li><a href="run-full-node.html">Full Node</a></li>
          <li><a href="sponsor-us.html">Sponsor Us</a></li>
        </ul>
      </div>
      <div>
        <h3>Introduction</h3>
        <ul style="padding-left:20px;margin:6px 0 0;">
          <li><a href="about-medorcoin.html">About MedorCoin</a></li>
          <li><a href="how-it-works.html">How it Works</a></li>
          <li><a href="important-to-know.html">Important to Know</a></li>
          <li><a href="introduction.html">Introduction</a></li>
          <li><a href="white-paper.html">White Paper</a></li>
        </ul>
      </div>
    </div>

    <div style="display:grid;grid-template-columns:1fr 1fr;gap:28px;margin-top:24px;">
      <div>
        <h3>Resources</h3>
        <ul style="padding-left:20px;margin:6px 0 0;">
          <li><a href="community.html">Community</a></li>
          <li><a href="latest-news.html">Latest News</a></li>
          <li><a href="resources.html">Resources</a></li>
          <li><a href="terms-and-conditions.html">Terms & Conditions</a></li>
        </ul>
      </div>
      <div>
        <h3>Who is it For</h3>
        <ul style="padding-left:20px;margin:6px 0 0;">
          <li><a href="institutions.html">For Institutions</a></li>
          <li><a href="investors.html">For Investors</a></li>
          <li><a href="medorcoin-core.html">MedorCoin Core</a></li>
        </ul>
      </div>
    </div>
  </section>

  <footer style="text-align:center;margin-top:40px;padding:12px 0;border-top:1px solid var(--border);color:#9eadb2;">
    <div>© MedorCoin 2026</div>
    <p style="font-size:12px;margin:6px 0 0;">
      Trade responsibly. The crypto world is volatile; you are responsible for your own decisions. MedorCoin and its partners are not liable for any decisions. You must be 18+ to use this platform.
    </p>
  </footer>

</div><!-- end .container -->

<script>
// FIX 6: detect wallet and show status badge
window.addEventListener("load", function() {
  const raw = localStorage.getItem("medorcoin_wallet");
  if (raw) {
    try {
      const w = JSON.parse(raw);
      const addr = w.address || "";
      const short = addr.slice(0, 6) + "..." + addr.slice(-4);
      const badge = document.getElementById("walletStatus");
      badge.textContent = "✓ Wallet: " + short + " — Open Dashboard";
      badge.style.display = "block";
    } catch {}
  }
});

// FIX 4: tile search filter
function filterTiles(val) {
  const q = val.toLowerCase();
  document.querySelectorAll("#tileGrid .tile").forEach(function(tile) {
    const text = tile.textContent.toLowerCase();
    tile.style.display = text.includes(q) ? "flex" : "none";
  });
}
</script>

</body>
</html>
