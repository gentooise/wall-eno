/**
 * HTML page for wall-eno status report.
 * Given its size, the page will remain in flash memory (PROGMEM)
 * and it will be read directly from there when needed.
 */
const char html_page[] PROGMEM = R"(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>wall-eno status</title>
  <style>
    body {
      margin: 0;
      padding: 0;
      font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
      background: linear-gradient(to right, #e8f5e9, #f1f8e9);
      color: #2e7d32;
      display: flex;
      flex-direction: column;
      align-items: center;
      justify-content: center;
      height: 100vh;
      text-align: center;
    }

    h1 {
      font-size: 2.2em;
      margin-bottom: 1em;
      display: flex;
      align-items: center;
      justify-content: center;
      gap: 0.2em; /* Reduced from 0.5em */
      animation: pulse 2.5s infinite;
    }

    h1 .emoji {
      margin-right: 0em;
      font-size: 1.2em;
    }

    .status {
      background-color: #ffffffcc;
      padding: 2em 2.5em;
      border-radius: 20px;
      box-shadow: 0 10px 20px rgba(76, 175, 80, 0.4);
      max-width: 90%;
    }

    table {
      border-collapse: collapse;
      margin: 0 auto;
      width: 100%;
    }

    td {
      padding: 0.6em 0.5em;
      vertical-align: middle;
    }

    .label {
      display: flex;
      align-items: center;
      justify-content: flex-start;
      font-weight: 500;
      font-size: 1.2em;
    }

    .label .emoji {
      font-size: 1.3em;
      margin-right: 0.4em;
    }

    .value {
      font-size: 1.3em;
      font-weight: bold;
      text-align: right;
      width: 5em;
    }

    .extra {
      font-size: 1em;
      color: #4caf50;
      text-align: left;
    }

    footer {
      margin-top: 2.5em;
      margin-bottom: 1em;
      font-size: 0.9em;
      color: #81c784;
    }

    @keyframes pulse {
      0% { transform: scale(1); opacity: 1; }
      50% { transform: scale(1.05); opacity: 0.75; }
      100% { transform: scale(1); opacity: 1; }
    }

    #error-block {
      margin-top: 1.5em;
      padding: 1em 1.4em;
      border-radius: 15px;
      background: #ffebee;
      color: #b71c1c;
      font-weight: 600;
      font-size: 1.1em;
      box-shadow: 0 6px 12px rgba(183, 28, 28, 0.25);
      border-left: 6px solid #e53935;
      border-right: 6px solid #e53935;
      display: block;     /* visible by default */
    }

    /* Hide completely when empty */
    #error-block:empty {
      display: none;
    }

    /* Optional: style emoji inside the block */
    #error-block .emoji {
      font-size: 1.3em;
      margin-right: 0.3em;
    }

    /* Auto-prepend emoji when error text exists */
    #error-block:not(:empty)::before {
      content: "⚠️";
      font-size: 1.3em;
      margin-right: 0.3em;
      line-height: 1;
    }
  </style>
</head>
<body>
  <div class="status">
    <h1><span class="emoji">⚡</span>wall-eno<span class="emoji">⚡</span></h1>
    <table>
      <tr>
        <td class="label"><span class="emoji">🏠</span> Home Consumption:</td>
        <td class="value"><span id="home-power">-</span> kW</td>
        <td class="extra">(raw: <span id="home-raw">-</span>)</td>
      </tr>
      <tr>
        <td class="label"><span class="emoji">🚗</span> Wallbox Limit:</td>
        <td class="value"><span id="wallbox-power">-</span> kW</td>
        <td class="extra">(<span id="wallbox-current">-</span> A)</td>
      </tr>
    </table>
    <div id="error-block"></div>
    <script>
        const UPDATE_INTERVAL = 6000;

        async function updateValues() {
            try {
                const response = await fetch("/wall-eno/json-status", { cache: "no-cache" });
                if (!response.ok) throw new Error(response.status);
                const data = await response.json();
                document.getElementById("home-power").textContent = data.homePower;
                document.getElementById("home-raw").textContent = data.homeRaw;
                document.getElementById("wallbox-power").textContent = data.wallboxPower;
                document.getElementById("wallbox-current").textContent = data.wallboxCurrent;
                document.getElementById("error-block").textContent = data.error;
            } catch (err) {
                document.getElementById("error-block").textContent = "Failed to update wall-eno status: " + err;
            }
        }

        updateValues();
        setInterval(updateValues, UPDATE_INTERVAL);
    </script>
    <footer>🌱 Smart charging powered by wall-eno 🤖</footer>
  </div>
</body>
</html>
)";
