#include "SettingsPageHtml.hpp"

/*
 * Settings UI served directly from flash. The page uses Bootstrap and jQuery
 * from public CDNs because it is intended for station-mode use on the local
 * network, not for the offline captive portal.
 */
extern const char SETTINGS_PAGE_HTML[] = R"HTML(<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>ADSB Radar</title>
  <link href="https://cdn.jsdelivr.net/npm/bootstrap@5.3.3/dist/css/bootstrap.min.css" rel="stylesheet">
  <link href="https://cdn.jsdelivr.net/npm/bootstrap-icons@1.11.3/font/bootstrap-icons.min.css" rel="stylesheet">
  <link href="https://unpkg.com/leaflet@1.9.4/dist/leaflet.css" rel="stylesheet">
  <link href="https://cdn.datatables.net/v/bs5/dt-2.1.8/rg-1.5.0/datatables.min.css" rel="stylesheet">
  <style>
    :root{--ag-primary:#3b7ddd;--ag-primary-dark:#2f64b1;--ag-sidebar:#222e3c;--ag-sidebar-2:#1c2633;--ag-body:#f5f7fb;--ag-text:#495057;--ag-heading:#233242;--ag-muted:#6c757d;--ag-line:#dee6ed;--ag-panel:#ffffff;--ag-soft:#f8f9fa;--ag-shadow:0 .1rem .2rem rgba(0,0,0,.05)}
    *{box-sizing:border-box}
    html,body{height:100%;min-height:100%;overflow:hidden}
    body{background:var(--ag-body);color:var(--ag-text);font-size:.875rem}
    .page-shell{max-width:none;padding:0!important}
    .app-header{background:#fff;border:0;border-bottom:1px solid #e9ecef;border-radius:0;box-shadow:var(--ag-shadow);color:var(--ag-text);padding:.85rem 1.5rem!important;margin:0!important;min-height:64px}
    .brand-logo{width:34px;height:34px;display:grid;place-items:center;border-radius:.25rem;background:#17202b;border:1px solid rgba(255,255,255,.08);flex:0 0 auto}
    .brand-logo svg{width:29px;height:29px;display:block}
    .brand-kicker,.small-label{font-size:.68rem;text-transform:uppercase;letter-spacing:.05em;color:var(--ag-muted);font-weight:700}
    .brand-title{font-weight:600;margin-bottom:0!important;color:var(--ag-heading);font-size:1rem}
    .brand-copy{color:var(--ag-muted);max-width:44rem;font-size:.825rem}
    .status-card{background:#f8f9fa;border:1px solid #e9ecef;border-radius:.25rem;padding:.55rem .75rem;min-width:150px}
    .sidebar-col{width:260px;flex:0 0 260px;transition:width .18s ease,flex-basis .18s ease}
    .content-col{width:calc(100% - 260px);flex:0 0 calc(100% - 260px);transition:width .18s ease,flex-basis .18s ease}
    .nav-card,.settings-card{border:0;border-radius:.25rem;background:#fff;box-shadow:var(--ag-shadow)}
    .nav-card{top:0;min-height:100vh;overflow:hidden;background:linear-gradient(180deg,var(--ag-sidebar),var(--ag-sidebar-2));box-shadow:none;border-radius:0}
    .nav-card .card-header{background:transparent;border-bottom:1px solid rgba(255,255,255,.08);padding:1.25rem 1rem}.nav-card .small-label{color:#adb5bd}
    .nav-brand{display:flex;align-items:center;gap:.75rem;color:#f8f9fa;font-size:1.05rem;font-weight:600;margin-bottom:.25rem}
    .nav-pills .nav-link{color:#ced4da;border-radius:.25rem;text-align:left;font-weight:500;padding:.72rem .9rem;transition:background .15s,color .15s;border:0;display:flex;align-items:center;gap:.75rem;white-space:nowrap}
    .nav-pills .nav-icon{width:1.25rem;min-width:1.25rem;text-align:center;font-size:1.05rem;line-height:1}
    .nav-pills .nav-label{overflow:hidden;text-overflow:ellipsis}
    .nav-pills .nav-link:hover{background:rgba(255,255,255,.06);color:#fff}
    .nav-pills .nav-link.active{background:#2f3d4f;color:#fff;box-shadow:inset 3px 0 0 var(--ag-primary)}
    .content-wrap{height:calc(100vh - 64px);padding:1.5rem;overflow:hidden}
    .page-title{font-size:1.55rem;font-weight:600;color:var(--ag-heading);margin:0 0 1.25rem}
    .settings-layout-card{height:100%;border:0;border-radius:.25rem;background:#fff;box-shadow:var(--ag-shadow);overflow:hidden}
    .settings-card{height:100%;overflow:hidden;box-shadow:none;border-left:1px solid #edf0f2;border-radius:0}
    .settings-card>.tab-pane{height:100%;padding:1.5rem;overflow-y:auto;overflow-x:hidden}
    .settings-card>.tab-pane.active{display:block}
    .tab-pane > .section-title{position:sticky;top:-1.5rem;z-index:10;display:flex;align-items:center;justify-content:space-between;gap:1rem;margin:-1.5rem -1.5rem 1.25rem;padding:1rem 1.5rem;background:#fff;border-bottom:1px solid #edf0f2}
    .section-actions{display:flex;flex-wrap:wrap;justify-content:flex-end;gap:.5rem;flex:0 0 auto}
    .section-title h2{font-size:1.25rem;margin:0;font-weight:600;color:var(--ag-heading)}
    .section-title p{margin:.25rem 0 0;color:var(--ag-muted)}
    .form-label{font-weight:500;color:#495057;margin-bottom:.35rem}
    .form-control,.form-select{border-color:#ced4da;border-radius:.25rem;background-color:#fff;min-height:38px}
    .form-control:hover,.form-select:hover{border-color:#adb5bd}
    .form-control:focus,.form-select:focus{border-color:#9fc2f3;box-shadow:0 0 0 .2rem rgba(59,125,221,.18)}
    .form-check.form-switch{padding:1rem 1rem 1rem 3.35rem;background:#fff;border:1px solid #e9ecef;border-radius:.25rem;min-height:58px}
    .form-switch .form-check-input{width:2.8em;height:1.45em;margin-left:-2.35rem}
    #displayPane .form-check.form-switch{display:flex;align-items:center;gap:.85rem;padding:1rem 1.1rem;min-height:58px}
    #displayPane .form-switch .form-check-input{margin-left:0;flex:0 0 auto}
    #displayPane .form-check-label{padding-left:.15rem;line-height:1.25}
    .form-check-input:checked{background-color:var(--ag-primary);border-color:var(--ag-primary)}
    .alert{border-radius:.25rem;border:1px solid #e9ecef}
    .editor-intro{background:#fff;border:1px solid #e9ecef;border-radius:.35rem;padding:1rem;box-shadow:var(--ag-shadow);margin-bottom:1rem}
    .editor-intro h3{font-size:1rem;margin:0;color:var(--ag-heading);font-weight:600}
    .editor-intro p{margin:.25rem 0 0;color:var(--ag-muted);line-height:1.45}
    .editor-grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:1rem}
    .range-row,.notify-row{background:#fff;border:1px solid #e9ecef;border-radius:.35rem;padding:1rem;box-shadow:var(--ag-shadow)}
    .range-row{display:flex;flex-direction:column;gap:1rem}
    .range-row .editor-card-head,.notify-row .editor-card-head{display:flex;align-items:flex-start;justify-content:space-between;gap:1rem;border-bottom:1px solid #edf0f2;padding-bottom:.75rem}
    .editor-card-title{display:flex;align-items:flex-start;gap:.75rem}
    .editor-card-icon{width:2.4rem;height:2.4rem;border-radius:.35rem;display:grid;place-items:center;background:#eef5ff;color:var(--ag-primary);font-size:1.15rem;flex:0 0 auto}
    .editor-card-title h3{font-size:1rem;margin:0;color:var(--ag-heading);font-weight:600}
    .editor-card-title p{margin:0;color:var(--ag-muted);line-height:1.45}
    .notify-row{display:flex;flex-direction:column;gap:1rem}
    .notify-row .form-switch{padding:.25rem 0 .25rem 2.75rem;background:transparent;border:0;min-height:auto}
    .notify-row .form-switch .form-check-input{margin-left:-2.75rem}
    .dashboard-panel{background:#fff;border:1px solid #dee2e6;border-top:0;border-radius:0 0 .25rem .25rem;padding:1rem}
    .dashboard-section{background:#fff;border:1px solid #e9ecef;border-radius:.35rem;padding:1rem;box-shadow:var(--ag-shadow);height:100%}
    .dashboard-section h2{font-size:1rem;font-weight:600;color:var(--ag-heading)}
    .dashboard-mini-card{border:1px solid #e9ecef;border-radius:.35rem;background:#fff;padding:1rem;height:100%;box-shadow:var(--ag-shadow)}
    .screenshot-frame{background:#07120b;border:1px solid #e9ecef;border-radius:.35rem;padding:1rem;text-align:center}
    .screenshot-section{position:relative;min-height:360px}
    .screenshot-loading{position:absolute;inset:0;z-index:20;display:none;align-items:center;justify-content:center;background:rgba(255,255,255,.82);backdrop-filter:blur(2px);border-radius:.35rem;cursor:wait}
    .screenshot-loading.active{display:flex}
    .screenshot-loading-box{background:#fff;border:1px solid #dee2e6;border-radius:.35rem;box-shadow:var(--ag-shadow);padding:1rem 1.25rem;min-width:260px;text-align:center;color:var(--ag-heading)}
    .style-group{border:1px solid #e9ecef;border-radius:.25rem;padding:1rem;margin-bottom:1rem;background:#fff}
    .style-tabs{border-bottom:1px solid #dee2e6;gap:.25rem}
    .style-tabs .nav-link{border:1px solid transparent;border-radius:.25rem .25rem 0 0;color:#495057;font-weight:500;padding:.6rem .85rem}
    .style-tabs .nav-link:hover{border-color:#e9ecef;background:#f8f9fa;color:var(--ag-primary)}
    .style-tabs .nav-link.active{background:#fff;border-color:#dee2e6 #dee2e6 #fff;color:var(--ag-primary)}
    .style-tab-content{background:#fff;border:1px solid #dee2e6;border-top:0;border-radius:0 0 .25rem .25rem;padding:1rem}
    .style-tab-content .style-group{border:0;border-radius:0;padding:0;margin:0;background:transparent}
    .display-card{background:#fff;border:1px solid #e9ecef;border-radius:.35rem;padding:1rem;box-shadow:var(--ag-shadow);height:100%}
    .display-card-head{display:flex;align-items:flex-start;gap:.75rem;margin-bottom:1rem}
    .display-card-head h3{font-size:1rem;margin:0;color:var(--ag-heading);font-weight:600}
    .display-card-head p{margin:0;color:var(--ag-muted);line-height:1.45}
    .style-row{border-top:1px solid #edf0f2;padding-top:.85rem;margin-top:.85rem}.style-row:first-of-type{border-top:0;margin-top:0;padding-top:0}
    .style-row .form-switch{padding:.35rem 0 .35rem 2.75rem;background:transparent;border:0;border-radius:0;min-height:auto}
    .metric-card{background:#fff;border:1px solid #e9ecef;border-radius:.25rem;padding:1rem;height:100%;box-shadow:var(--ag-shadow)}
    .metric-value{font-size:1.45rem;font-weight:600;color:var(--ag-heading);line-height:1.15}
    .location-panel{background:#fff;border:1px solid #e9ecef;border-radius:.25rem;padding:1rem;height:100%}
    .location-source-grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:.75rem}
    .location-source{border:1px solid #dee2e6;border-radius:.35rem;padding:1rem;cursor:pointer;background:#fff;min-height:122px;transition:border-color .15s,box-shadow .15s,background .15s,transform .15s}
    .location-source:hover{border-color:#9fc2f3;background:#f8fbff;transform:translateY(-1px)}
    .location-source.active{border-color:var(--ag-primary);box-shadow:inset 3px 0 0 var(--ag-primary),var(--ag-shadow);background:#f8fbff}
    .location-source .source-top{display:flex;align-items:flex-start;justify-content:space-between;gap:.75rem}
    .location-source .source-icon{width:2.4rem;height:2.4rem;border-radius:.35rem;display:grid;place-items:center;background:#eef5ff;color:var(--ag-primary);font-size:1.2rem;flex:0 0 auto}
    .location-source h3{font-size:1rem;margin:0;color:var(--ag-heading);font-weight:600}
    .location-source p{margin:0;color:var(--ag-muted);line-height:1.45}
    .location-source .form-check-input{margin-top:.15rem;flex:0 0 auto}
    .data-source-grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:1rem}
    .data-source-card{position:relative;border:1px solid #dee2e6;border-radius:.35rem;background:#fff;padding:1rem;cursor:pointer;min-height:178px;display:flex;flex-direction:column;gap:.75rem;transition:border-color .15s,box-shadow .15s,background .15s,transform .15s}
    .data-source-card:hover{border-color:#9fc2f3;background:#f8fbff;transform:translateY(-1px)}
    .data-source-card.active{border-color:var(--ag-primary);box-shadow:inset 3px 0 0 var(--ag-primary),var(--ag-shadow);background:#f8fbff}
    .data-source-card .source-top{display:flex;align-items:flex-start;justify-content:space-between;gap:.75rem}
    .data-source-card .source-icon{width:2.4rem;height:2.4rem;border-radius:.35rem;display:grid;place-items:center;background:#eef5ff;color:var(--ag-primary);font-size:1.2rem;flex:0 0 auto}
    .data-source-card h3{font-size:1rem;margin:0;color:var(--ag-heading);font-weight:600}
    .data-source-card p{margin:0;color:var(--ag-muted);line-height:1.45}
    .source-badges{display:flex;flex-wrap:wrap;gap:.35rem;margin-top:auto}
    .source-badge{border:1px solid #d8e4f3;background:#f5f9ff;color:#315f97;border-radius:999px;padding:.2rem .55rem;font-size:.72rem;font-weight:600}
    .data-source-card .form-check-input{margin-top:.15rem}
    .service-card{height:100%;background:#fff;border:1px solid #e9ecef;border-radius:.35rem;padding:1rem;box-shadow:var(--ag-shadow)}
    .service-card h3{font-size:1rem;margin:0;color:var(--ag-heading);font-weight:600}
    .service-card .service-icon{width:2.25rem;height:2.25rem;border-radius:.35rem;background:#f8f9fa;border:1px solid #e9ecef;display:grid;place-items:center;color:#495057}
    .source-helper{background:#f8f9fa;border:1px solid #e9ecef;border-radius:.35rem;padding:1rem}
    #locationMap{height:520px;min-height:360px;border-radius:.25rem;border:1px solid #dee2e6;overflow:hidden;background:#e9ecef}
    .map-readout{border:1px solid #e9ecef;border-radius:.25rem;background:#f8f9fa;padding:.75rem}
    .leaflet-container{font:inherit}
    .status-table td,.status-table th{vertical-align:middle}
    .table{--bs-table-bg:transparent}.table thead th{color:#6c757d;font-size:.72rem;text-transform:uppercase;letter-spacing:.04em;border-bottom-color:#dee2e6}.table tbody td,.table tbody th{border-bottom-color:#edf0f2}
    .aircraft-row-displayed{background:rgba(59,125,221,.06)}
    .aircraft-row-labelled{background:rgba(59,125,221,.16)}
    .aircraft-row-labelled th:first-child{box-shadow:inset 3px 0 0 var(--ag-primary)}
    tr.dtrg-group th{background:#f8f9fa!important;color:#233242;font-weight:600}
    .list-group-item{border-color:#dee2e6}
    .form-control-color{width:3.4rem;padding:.22rem}
    .width-control{max-width:110px}.ui-control{max-width:140px}
    .btn{border-radius:.25rem;font-weight:500;min-height:38px}.btn-primary{background:var(--ag-primary);border-color:var(--ag-primary)}.btn-primary:hover,.btn-primary:focus{background:var(--ag-primary-dark);border-color:var(--ag-primary-dark)}.btn-outline-primary{color:var(--ag-primary);border-color:var(--ag-primary)}.btn-outline-primary:hover{background:var(--ag-primary);border-color:var(--ag-primary)}.btn-outline-secondary{border-color:#ced4da;color:#495057}.btn-outline-secondary:hover{background:#f8f9fa;border-color:#adb5bd;color:#212529}
    .badge{font-weight:600}
    @media (max-width:1199.98px){.sidebar-col{width:76px;flex-basis:76px}.content-col{width:calc(100% - 76px);flex-basis:calc(100% - 76px)}.nav-card .card-header{padding:1rem .65rem}.nav-brand{justify-content:center;margin-bottom:0}.nav-brand span,.nav-card .small-label,.nav-pills .nav-label{display:none}.nav-pills .nav-link{justify-content:center;padding:.85rem .5rem}.nav-pills .nav-link.active{box-shadow:inset 0 0 0 1px rgba(59,125,221,.55)}.nav-pills .nav-icon{font-size:1.2rem}}
    @media (max-width:991.98px){html,body{height:auto;min-height:100%;overflow:auto}.nav-card{min-height:100vh;border-radius:0}.content-wrap{height:auto;min-height:calc(100vh - 64px);padding:1rem;overflow:visible}.settings-layout-card{height:auto}.settings-card{height:auto}.settings-card>.tab-pane{height:auto;max-height:none}.brand-logo{width:34px;height:34px}.brand-logo svg{width:29px;height:29px}.settings-card{border-left:1px solid #edf0f2;border-top:0}#locationMap{height:420px}.location-source-grid,.data-source-grid,.editor-grid{grid-template-columns:1fr}}
    @media (max-width:575.98px){.sidebar-col{width:62px;flex-basis:62px}.content-col{width:calc(100% - 62px);flex-basis:calc(100% - 62px)}.nav-card .card-header{padding:.75rem .45rem}.nav-card .card-body{padding:.35rem!important}.nav-pills .nav-link{padding:.75rem .35rem}.settings-card>.tab-pane{padding:1rem}.app-header{border-radius:0}}
  </style>
</head>
<body>
<main class="container-fluid page-shell">
  <div class="row g-0 align-items-stretch">
    <aside class="sidebar-col">
      <div class="card nav-card">
        <div class="card-header">
          <div class="nav-brand">
            <div class="brand-logo" aria-hidden="true">
              <svg viewBox="0 0 100 100" role="img">
                <circle cx="50" cy="50" r="44" fill="#0b1723" stroke="#3b7ddd" stroke-width="3"/>
                <circle cx="50" cy="50" r="31" fill="none" stroke="#6ea8fe" stroke-width="2" opacity=".7"/>
                <circle cx="50" cy="50" r="18" fill="none" stroke="#6ea8fe" stroke-width="2" opacity=".45"/>
                <path d="M50 50 L79 24" stroke="#8cff9e" stroke-width="4" stroke-linecap="round"/>
                <path d="M50 8 V92 M8 50 H92" stroke="#6ea8fe" stroke-width="1.5" opacity=".35"/>
                <circle cx="34" cy="38" r="4" fill="#facc15"/>
                <circle cx="69" cy="61" r="4" fill="#5eead4"/>
              </svg>
            </div>
            <span>ADSB Radar</span>
          </div>
          <span class="small-label">Configuration</span>
        </div>
        <div class="card-body p-2">
          <ul class="nav nav-pills flex-lg-column gap-2" id="settingsTabs" role="tablist">
            <li class="nav-item"><button class="nav-link active w-100" data-bs-toggle="tab" data-bs-target="#espStatus" type="button" title="Dashboard"><i class="bi bi-speedometer2 nav-icon" aria-hidden="true"></i><span class="nav-label">Dashboard</span></button></li>
            <li class="nav-item"><button class="nav-link w-100" data-bs-toggle="tab" data-bs-target="#general" type="button" title="Location"><i class="bi bi-geo-alt nav-icon" aria-hidden="true"></i><span class="nav-label">Location</span></button></li>
            <li class="nav-item"><button class="nav-link w-100" data-bs-toggle="tab" data-bs-target="#apiKeys" type="button" title="Data Sources"><i class="bi bi-database nav-icon" aria-hidden="true"></i><span class="nav-label">Data Sources</span></button></li>
            <li class="nav-item"><button class="nav-link w-100" data-bs-toggle="tab" data-bs-target="#colours" type="button" title="Display"><i class="bi bi-palette nav-icon" aria-hidden="true"></i><span class="nav-label">Display</span></button></li>
            <li class="nav-item"><button class="nav-link w-100" data-bs-toggle="tab" data-bs-target="#notifications" type="button" title="Notifications"><i class="bi bi-bell nav-icon" aria-hidden="true"></i><span class="nav-label">Notifications</span></button></li>
            <li class="nav-item"><button class="nav-link w-100" data-bs-toggle="tab" data-bs-target="#ranges" type="button" title="Ranges"><i class="bi bi-broadcast-pin nav-icon" aria-hidden="true"></i><span class="nav-label">Ranges</span></button></li>
            <li class="nav-item"><button class="nav-link w-100" data-bs-toggle="tab" data-bs-target="#wifi" type="button" title="WiFi"><i class="bi bi-wifi nav-icon" aria-hidden="true"></i><span class="nav-label">WiFi</span></button></li>
          </ul>
        </div>
      </div>
    </aside>
    <div class="content-col">
      <div class="app-header d-flex flex-wrap align-items-center justify-content-end gap-3">
        <div class="status-card">
          <span class="brand-kicker me-2">Save status</span>
          <span id="saveStatus" class="badge rounded-pill text-bg-secondary">Loading</span>
        </div>
      </div>
      <div class="content-wrap">
        <div class="settings-layout-card">
          <div class="tab-content settings-card card">
    <section class="tab-pane fade" id="general">
      <div class="section-title">
        <div><h2>Radar centre and startup range</h2><p>Choose the radar origin and the range used when the device starts.</p></div>
        <div class="section-actions"><button class="btn btn-primary save-settings">Save Location</button></div>
      </div>
      <div class="row g-4">
        <div class="col-xl-5">
          <div class="location-panel">
            <label class="form-label">Radar centre source</label>
            <select id="centerSource" class="form-select visually-hidden" aria-label="Radar centre source">
              <option value="manual">Manual position</option>
              <option value="gps">USB GPS</option>
              <option value="airport">Airport</option>
              <option value="location">Search by location</option>
            </select>
            <div class="location-source-grid mb-4">
              <div class="location-source" data-source="manual">
                <div class="source-top">
                  <div class="d-flex gap-3">
                    <div class="source-icon"><i class="bi bi-crosshair"></i></div>
                    <div><h3>Manual</h3><p>Click the map or enter exact coordinates.</p></div>
                  </div>
                  <input class="form-check-input" name="locationSourceCard" type="radio" value="manual" id="sourceManual" aria-label="Use manual position">
                </div>
              </div>
              <div class="location-source" data-source="gps">
                <div class="source-top">
                  <div class="d-flex gap-3">
                    <div class="source-icon"><i class="bi bi-usb-symbol"></i></div>
                    <div><h3>USB GPS</h3><p>Use the current GPS fix from the USB receiver.</p></div>
                  </div>
                  <input class="form-check-input" name="locationSourceCard" type="radio" value="gps" id="sourceGps" aria-label="Use USB GPS">
                </div>
              </div>
              <div class="location-source" data-source="airport">
                <div class="source-top">
                  <div class="d-flex gap-3">
                    <div class="source-icon"><i class="bi bi-airplane-engines"></i></div>
                    <div><h3>Airport</h3><p>Search the onboard airport database.</p></div>
                  </div>
                  <input class="form-check-input" name="locationSourceCard" type="radio" value="airport" id="sourceAirport" aria-label="Use airport centre">
                </div>
              </div>
              <div class="location-source" data-source="location">
                <div class="source-top">
                  <div class="d-flex gap-3">
                    <div class="source-icon"><i class="bi bi-geo-alt"></i></div>
                    <div><h3>Place search</h3><p>Find a town, city, or named place.</p></div>
                  </div>
                  <input class="form-check-input" name="locationSourceCard" type="radio" value="location" id="sourceLocation" aria-label="Use place search">
                </div>
              </div>
            </div>
            <div class="row g-3">
              <div class="col-md-6"><label class="form-label">Latitude</label><input id="lat" type="number" step="0.000001" class="form-control"></div>
              <div class="col-md-6"><label class="form-label">Longitude</label><input id="lon" type="number" step="0.000001" class="form-control"></div>
              <div class="col-md-12"><label class="form-label">Startup range</label><select id="defaultRange" class="form-select"></select></div>
            </div>
            <div id="gpsStatusBox" class="alert alert-secondary mt-4 mb-0">
              <div class="fw-semibold" id="gpsStatus">GPS status unknown</div>
              <div id="gpsDetail" class="small"></div>
              <button id="copyGpsToManual" class="btn btn-sm btn-outline-primary mt-2" type="button" disabled>Copy GPS to manual</button>
            </div>
            <div id="airportBox" class="mt-4">
              <label class="form-label" for="airportSearch">Airport centre</label>
              <input id="airportSearch" class="form-control" list="airportOptions" autocomplete="off" placeholder="Start typing an airport name or ICAO">
              <datalist id="airportOptions"></datalist>
              <div id="airportDetail" class="form-text"></div>
              <button id="copyAirportToManual" class="btn btn-sm btn-outline-primary mt-2" type="button" disabled>Copy airport to manual</button>
            </div>
            <div id="locationBox" class="mt-4">
              <label class="form-label" for="locationSearch">Location centre</label>
              <div class="input-group">
                <input id="locationSearch" class="form-control" autocomplete="off" placeholder="Search city, town, or place">
                <button id="searchLocation" class="btn btn-outline-primary" type="button">Search</button>
              </div>
              <div id="locationResults" class="list-group mt-3"></div>
              <div id="locationDetail" class="form-text"></div>
              <button id="copyLocationToManual" class="btn btn-sm btn-outline-primary mt-2" type="button" disabled>Copy location to manual</button>
            </div>
          </div>
        </div>
        <div class="col-xl-7">
          <div class="location-panel">
            <div class="d-flex flex-wrap justify-content-between align-items-start gap-3 mb-3">
              <div>
                <div class="small-label">Selected radar centre</div>
                <div id="mapTitle" class="h5 mb-1">Manual position</div>
                <div id="mapHelp" class="text-muted small">Click the map to set the manual radar centre.</div>
              </div>
              <div class="map-readout text-end">
                <div class="small-label">Coordinates</div>
                <div id="mapCoords" class="fw-semibold">--</div>
              </div>
            </div>
            <div id="locationMap"></div>
            <div class="text-muted small mt-2">Map data &copy; OpenStreetMap contributors.</div>
          </div>
        </div>
      </div>
    </section>
    <section class="tab-pane fade" id="apiKeys">
      <div class="section-title">
        <div><h2>Aircraft feeds and external APIs</h2><p>Choose the live aircraft feed and manage optional service keys used by the radar.</p></div>
        <div class="section-actions"><button class="btn btn-primary save-settings">Save Data Sources</button></div>
      </div>
      <select id="dataSource" class="form-select visually-hidden" aria-label="Aircraft data source">
        <option value="airplanes_live">Airplanes.live</option>
        <option value="adsb_lol">ADSB.lol</option>
        <option value="adsb_fi">ADSB.fi</option>
        <option value="local">Local aircraft.json</option>
      </select>
      <ul class="nav style-tabs mb-0" role="tablist">
        <li class="nav-item" role="presentation"><button class="nav-link active" data-bs-toggle="tab" data-bs-target="#dataSourceAircraft" type="button">Aircraft Source</button></li>
        <li class="nav-item" role="presentation"><button class="nav-link" data-bs-toggle="tab" data-bs-target="#dataSourceKeys" type="button">Service Keys</button></li>
      </ul>
      <div class="tab-content style-tab-content">
        <div class="tab-pane fade show active" id="dataSourceAircraft">
          <div class="d-flex align-items-center justify-content-between flex-wrap gap-2 mb-3">
            <div>
              <div class="small-label">Aircraft source</div>
              <h3 class="h5 mb-0">Live traffic feed</h3>
            </div>
            <div class="text-muted small">Internet feeds use the radar centre and selected range. Local mode reads your own receiver.</div>
          </div>
          <div class="data-source-grid">
            <div class="data-source-card" data-source="airplanes_live">
              <div class="source-top">
                <div class="d-flex gap-3">
                  <div class="source-icon"><i class="bi bi-broadcast-pin"></i></div>
                  <div><h3>Airplanes.live</h3><p>Primary public aircraft feed using the existing point query endpoint.</p></div>
                </div>
                <input class="form-check-input" name="dataSourceCard" type="radio" value="airplanes_live" aria-label="Use Airplanes.live">
              </div>
              <div class="source-badges"><span class="source-badge">Internet</span><span class="source-badge">Range query</span><span class="source-badge">No key</span></div>
            </div>
            <div class="data-source-card" data-source="adsb_lol">
              <div class="source-top">
                <div class="d-flex gap-3">
                  <div class="source-icon"><i class="bi bi-cloud-arrow-down"></i></div>
                  <div><h3>ADSB.lol</h3><p>Alternative public feed with the same point URL shape as Airplanes.live.</p></div>
                </div>
                <input class="form-check-input" name="dataSourceCard" type="radio" value="adsb_lol" aria-label="Use ADSB.lol">
              </div>
              <div class="source-badges"><span class="source-badge">Internet</span><span class="source-badge">Range query</span><span class="source-badge">No key</span></div>
            </div>
            <div class="data-source-card" data-source="adsb_fi">
              <div class="source-top">
                <div class="d-flex gap-3">
                  <div class="source-icon"><i class="bi bi-globe2"></i></div>
                  <div><h3>ADSB.fi</h3><p>Optional open data feed using the ADSB.fi latitude, longitude, and distance API.</p></div>
                </div>
                <input class="form-check-input" name="dataSourceCard" type="radio" value="adsb_fi" aria-label="Use ADSB.fi">
              </div>
              <div class="source-badges"><span class="source-badge">Internet</span><span class="source-badge">Open data</span><span class="source-badge">No key</span></div>
            </div>
            <div class="data-source-card" data-source="local">
              <div class="source-top">
                <div class="d-flex gap-3">
                  <div class="source-icon"><i class="bi bi-hdd-network"></i></div>
                  <div><h3>Local receiver</h3><p>Read a dump1090 or readsb compatible aircraft.json feed on your own network.</p></div>
                </div>
                <input class="form-check-input" name="dataSourceCard" type="radio" value="local" aria-label="Use local aircraft JSON">
              </div>
              <div class="source-badges"><span class="source-badge">LAN</span><span class="source-badge">Standalone receiver</span><span class="source-badge">URL required</span></div>
              <div id="localSourcePanel" class="source-helper mt-1">
                <label class="form-label">Local aircraft JSON URL</label>
                <input id="localAircraftUrl" type="url" maxlength="191" class="form-control" placeholder="http://192.168.1.28:8080/data/aircraft.json">
                <div class="form-text">Only used by Local receiver mode.</div>
              </div>
            </div>
          </div>
        </div>
        <div class="tab-pane fade" id="dataSourceKeys">
          <div class="d-flex align-items-center justify-content-between flex-wrap gap-2 mb-3">
            <div>
              <div class="small-label">Service keys</div>
              <h3 class="h5 mb-0">External lookup services</h3>
            </div>
            <div class="text-muted small">These keys are only used by optional lookup features.</div>
          </div>
          <div class="row g-3">
            <div class="col-lg-6">
              <div class="service-card">
                <div class="d-flex align-items-start gap-3 mb-3">
                  <div class="service-icon"><i class="bi bi-signpost-split"></i></div>
                  <div><h3>AirportDB</h3><p class="text-muted mb-0">Required only when runway drawing is enabled for an airport-centred radar.</p></div>
                </div>
                <label class="form-label" for="airportDbToken">API key</label>
                <input id="airportDbToken" type="text" maxlength="191" class="form-control" autocomplete="off" placeholder="airportdb.io API token">
                <div class="form-text">AirportDB registration is separate and the public plan has a monthly request limit.</div>
              </div>
            </div>
            <div class="col-lg-6">
              <div class="service-card">
                <div class="d-flex align-items-start gap-3 mb-3">
                  <div class="service-icon"><i class="bi bi-geo-alt"></i></div>
                  <div><h3>OpenWeather Geocoding</h3><p class="text-muted mb-0">Used by Search by location to turn place names into latitude and longitude.</p></div>
                </div>
                <label class="form-label" for="openWeatherApiKey">API key</label>
                <input id="openWeatherApiKey" type="text" maxlength="95" class="form-control" autocomplete="off" placeholder="OpenWeather API key">
                <div class="form-text">Not required for manual, GPS, or airport centre selection.</div>
              </div>
            </div>
          </div>
        </div>
      </div>
    </section>
    <section class="tab-pane fade" id="colours">
      <div class="section-title">
        <div><h2>Visual elements</h2><p>Tune colour, visibility, line width, tick spacing, and radar geometry by group.</p></div>
        <div class="section-actions">
          <button class="btn btn-primary save-settings">Save Display</button>
          <button id="resetAppearance" class="btn btn-outline-secondary" type="button">Reset all display styling</button>
        </div>
      </div>
      <div id="styleGroups"></div>
    </section>
    <section class="tab-pane fade" id="notifications">
      <div class="section-title">
        <div><h2>Aircraft type alerts</h2><p>Highlight matching aircraft types and show alert text on the radar display.</p></div>
        <div class="section-actions"><button class="btn btn-primary save-settings">Save Notifications</button></div>
      </div>
      <div class="editor-intro">
        <div class="d-flex align-items-start gap-3">
          <div class="editor-card-icon"><i class="bi bi-bell"></i></div>
          <div><h3>Notification rules</h3><p>Match aircraft by type substring, colour them on the radar, keep their labels visible, and show optional banner text.</p></div>
        </div>
      </div>
      <div id="notificationRows" class="editor-grid"></div>
    </section>
    <section class="tab-pane fade" id="ranges">
      <div class="section-title">
        <div><h2>Range presets and refresh rates</h2><p>Set the selectable ranges, data refresh intervals, and label density.</p></div>
        <div class="section-actions">
          <button class="btn btn-primary save-settings">Save Ranges</button>
          <button id="resetRanges" class="btn btn-outline-secondary" type="button">Reset ranges</button>
        </div>
      </div>
      <div class="editor-intro">
        <div class="d-flex align-items-start gap-3">
          <div class="editor-card-icon"><i class="bi bi-bullseye"></i></div>
          <div><h3>Selectable radar ranges</h3><p>Each preset controls the range shown on the device, how often aircraft data refreshes, and how many labels can be drawn.</p></div>
        </div>
      </div>
      <div id="rangeRows" class="editor-grid"></div>
    </section>
    <section class="tab-pane fade" id="wifi">
      <div class="section-title">
        <div><h2>Network connection</h2><p>Scan for nearby networks and store credentials on the device.</p></div>
        <div class="section-actions">
          <button id="scanWifi" class="btn btn-outline-primary" type="button">Scan</button>
          <button id="saveWifi" class="btn btn-primary" type="button">Save WiFi</button>
        </div>
      </div>
      <div class="mb-3"><span class="small-label">Current network</span><div id="currentWifi" class="fw-semibold"></div></div>
      <div class="row g-3">
        <div class="col-md-5"><label class="form-label">Network</label><select id="wifiSsid" class="form-select"><option value="">Scan to load networks</option></select></div>
        <div class="col-md-4"><label class="form-label">Manual SSID</label><input id="wifiManual" maxlength="32" class="form-control"></div>
        <div class="col-md-3"><label class="form-label">Password</label><input id="wifiPassword" type="password" maxlength="64" class="form-control"></div>
      </div>
    </section>
    <section class="tab-pane fade show active" id="espStatus">
      <div class="section-title">
        <div><h2>Dashboard</h2><p>Live device telemetry, heap usage, active caches, network state, and data pipeline status.</p></div>
        <div class="section-actions"><button id="refreshDeviceStatus" class="btn btn-outline-primary" type="button">Refresh</button></div>
      </div>
      <ul class="nav style-tabs mb-0" id="statusSubTabs" role="tablist">
        <li class="nav-item" role="presentation"><button class="nav-link active" data-bs-toggle="tab" data-bs-target="#statusInfo" type="button" role="tab">Info</button></li>
        <li class="nav-item" role="presentation"><button class="nav-link" data-bs-toggle="tab" data-bs-target="#statusAircraft" type="button" role="tab">Aircraft</button></li>
        <li class="nav-item" role="presentation"><button class="nav-link" data-bs-toggle="tab" data-bs-target="#statusScreenshot" type="button" role="tab">Screenshot</button></li>
        <li class="nav-item" role="presentation"><button class="nav-link" data-bs-toggle="tab" data-bs-target="#statusImportExport" type="button" role="tab">Import / Export</button></li>
      </ul>
      <div class="tab-content dashboard-panel">
        <div class="tab-pane fade show active" id="statusInfo" role="tabpanel">
          <div id="deviceStatusSummary" class="row g-3 mb-4"></div>
          <div class="row g-4">
            <div class="col-12">
              <div class="dashboard-section">
                <div class="d-flex justify-content-between align-items-center mb-2">
                  <h2 class="h5 mb-0">Memory</h2>
                  <span id="deviceStatusTime" class="text-muted small">Not loaded</span>
                </div>
                <div class="table-responsive">
                  <table class="table table-sm status-table mb-0">
                    <thead><tr><th>Heap</th><th>Used</th><th>Free</th><th>Largest</th><th>Low water</th></tr></thead>
                    <tbody id="memoryStatusRows"><tr><td colspan="5" class="text-muted">Load status to view memory.</td></tr></tbody>
                  </table>
                </div>
              </div>
            </div>
            <div class="col-12">
              <div class="dashboard-section">
                <h2 class="h5 mb-2">Caches and buffers</h2>
                <div class="table-responsive">
                  <table class="table table-sm status-table mb-0">
                    <thead><tr><th>Name</th><th>Status</th><th>Memory</th></tr></thead>
                    <tbody id="cacheStatusRows"><tr><td colspan="3" class="text-muted">Load status to view cache usage.</td></tr></tbody>
                  </table>
                </div>
              </div>
            </div>
            <div class="col-12">
              <div class="dashboard-section">
                <h2 class="h5 mb-2">GPS</h2>
                <div id="gpsStatusDetail" class="row g-3"></div>
              </div>
            </div>
            <div class="col-12">
              <div class="dashboard-section">
                <h2 class="h5 mb-2">WiFi</h2>
                <div id="wifiStatusDetail" class="row g-3"></div>
              </div>
            </div>
          </div>
        </div>
        <div class="tab-pane fade" id="statusAircraft" role="tabpanel">
          <div id="aircraftStatusSummary" class="row g-3 mb-4"></div>
          <div class="dashboard-section">
            <div class="d-flex flex-wrap justify-content-between align-items-center gap-2 mb-2">
              <h2 class="h5 mb-0">Tracked aircraft</h2>
              <span id="aircraftStatusTime" class="text-muted small">Not loaded</span>
            </div>
            <div class="table-responsive">
              <table id="aircraftStatusTable" class="table table-sm status-table mb-0">
                <thead><tr><th>Callsign</th><th>Display group</th><th>Status</th><th>ICAO</th><th>Type</th><th>Distance</th><th>Bearing</th><th>Altitude</th><th>Speed</th><th>Seen</th><th></th></tr></thead>
                <tbody id="aircraftStatusRows"><tr><td colspan="11" class="text-muted">Load status to view aircraft.</td></tr></tbody>
              </table>
            </div>
          </div>
        </div>
        <div class="tab-pane fade" id="statusScreenshot" role="tabpanel">
          <div class="dashboard-section screenshot-section">
            <div id="screenshotLoading" class="screenshot-loading" aria-live="polite" aria-busy="true">
              <div class="screenshot-loading-box">
                <div class="spinner-border text-primary mb-3" role="status" aria-hidden="true"></div>
                <div class="fw-semibold">Preparing screenshot</div>
                <div class="text-muted small">Capturing the current LVGL display...</div>
              </div>
            </div>
            <div class="d-flex flex-wrap justify-content-between align-items-center gap-2 mb-3">
              <div>
                <h2 class="h5 mb-1">Radar screenshot</h2>
                <div id="screenshotStatus" class="text-muted small">Load a fresh capture from the ESP32 display.</div>
              </div>
              <div class="d-flex gap-2">
                <button id="refreshScreenshot" class="btn btn-outline-primary" type="button">Refresh capture</button>
                <a id="downloadScreenshot" class="btn btn-primary" href="/api/screenshot.png" download="adsb-radar-screenshot.png">Download PNG</a>
              </div>
            </div>
            <div class="screenshot-frame">
              <img id="radarScreenshot" class="img-fluid border border-secondary" alt="Current radar screenshot" style="max-height:720px;border-radius:50%;display:none">
            </div>
          </div>
        </div>
        <div class="tab-pane fade" id="statusImportExport" role="tabpanel">
          <div class="row g-4">
            <div class="col-xl-6">
              <div class="dashboard-section">
                <div class="d-flex flex-wrap justify-content-between align-items-center gap-2 mb-3">
                  <div>
                    <h2 class="h5 mb-1">Export settings</h2>
                    <p class="text-muted small mb-0">Download the current radar configuration as named JSON fields, including service keys and display settings.</p>
                  </div>
                  <button id="exportSettings" class="btn btn-primary" type="button">Download JSON</button>
                </div>
                <div class="alert alert-light border mb-0">
                  <div class="small-label">Export format</div>
                  <div class="small">The file contains a version number and a <code>settings</code> object. It does not include live status such as GPS lock, heap usage, or tracked aircraft.</div>
                </div>
              </div>
            </div>
            <div class="col-xl-6">
              <div class="dashboard-section">
                <div class="d-flex flex-wrap justify-content-between align-items-center gap-2 mb-3">
                  <div>
                    <h2 class="h5 mb-1">Import settings</h2>
                    <p class="text-muted small mb-0">Import a JSON export or paste a settings object. Version mismatches are warned about but can still be imported.</p>
                  </div>
                  <button id="importSettings" class="btn btn-primary" type="button">Import JSON</button>
                </div>
                <div id="importExportStatus" class="alert alert-secondary py-2 mb-3">No import loaded.</div>
                <div class="mb-3">
                  <label class="form-label" for="importSettingsFile">JSON file</label>
                  <input id="importSettingsFile" class="form-control" type="file" accept="application/json,.json">
                </div>
                <div>
                  <label class="form-label" for="importSettingsText">JSON text</label>
                  <textarea id="importSettingsText" class="form-control font-monospace" rows="10" spellcheck="false" placeholder='{"version":1,"settings":{...}}'></textarea>
                </div>
              </div>
            </div>
          </div>
        </div>
      </div>
    </section>
      </div>
    </div>
    </div>
    </div>
  </div>
</main>
<script src="https://code.jquery.com/jquery-3.7.1.min.js"></script>
<script src="https://cdn.jsdelivr.net/npm/bootstrap@5.3.3/dist/js/bootstrap.bundle.min.js"></script>
<script src="https://unpkg.com/leaflet@1.9.4/dist/leaflet.js"></script>
<script src="https://cdn.datatables.net/v/bs5/dt-2.1.8/rg-1.5.0/datatables.min.js"></script>
<script>
const SETTINGS_EXPORT_VERSION=1;
let cfg=null, defaults=null, selectedAirport=null, selectedLocation=null, latestGps=null, airportOptions={}, airportTimer=null, locationResults=[], aircraftTable=null, locationMap=null, locationMarker=null;
const styleDefs={
  screen_bg:{label:'Screen background'},radar_bg:{label:'Radar background'},radar_glow:{label:'Radar centre glow'},portal_bg:{label:'WiFi setup background'},
  radar_bright:{label:'Range rings / major ticks',width:true,visible:true,ui:[{key:'major_tick_length',label:'Major tick length',min:1,max:80},{key:'major_tick_degrees',label:'Major tick angle',min:1,max:90}]},
  radar_grid:{label:'Square grid lines',width:true,visible:true},
  radar_radial:{label:'Radial lines',width:true,visible:true,ui:[{key:'radial_degrees',label:'Degrees between radials',min:1,max:90}]},
  radar_tick_medium:{label:'Medium ticks',width:true,visible:true,ui:[{key:'medium_tick_length',label:'Medium tick length',min:1,max:80},{key:'medium_tick_degrees',label:'Medium tick angle',min:1,max:90}]},
  radar_tick_minor:{label:'Minor ticks',width:true,visible:true,ui:[{key:'minor_tick_length',label:'Minor tick length',min:1,max:80},{key:'minor_tick_degrees',label:'Minor tick angle',min:1,max:90}]},
  radar_axis:{label:'Main axis lines',width:true,visible:true},
  country_boundary:{label:'Country outlines',width:true,visible:true},
  airport:{label:'Airport markers',width:true,visible:true},
  runway:{label:'Airport runways',width:true,visible:true},
  sweep:{label:'Sweep line',width:true,visible:true},
  control_arc:{label:'Button arcs',width:true,visible:true},range_label:{label:'Range labels'},heading_label:{label:'Compass labels'},button_text:{label:'Button text'},button_status:{label:'Status button text'},button_pressed:{label:'Button press fill'},
  aircraft_normal:{label:'Aircraft fallback'},aircraft_stale:{label:'Aircraft stale'},aircraft_low:{label:'Aircraft low fallback'},aircraft_emergency:{label:'Aircraft emergency'},aircraft_hit:{label:'Sweep highlight'},climb_triangle:{label:'Climb triangle'},descent_triangle:{label:'Descent triangle'},aircraft_heading:{label:'Aircraft heading arrows',width:true,visible:true,color:false},
  ground:{label:'Ground / altitude 0',source:'altitudeColors'},below_2000:{label:'Below 2,000 ft',source:'altitudeColors'},below_10000:{label:'2,000-9,999 ft',source:'altitudeColors'},below_20000:{label:'10,000-19,999 ft',source:'altitudeColors'},below_30000:{label:'20,000-29,999 ft',source:'altitudeColors'},below_40000:{label:'30,000-39,999 ft',source:'altitudeColors'},above_40000:{label:'40,000 ft and above',source:'altitudeColors'},
  text_primary:{label:'Primary text'},text_secondary:{label:'Secondary text'},popup_bg:{label:'Popup background'},popup_border:{label:'Popup border'},
  gps_neutral:{label:'GPS neutral'},gps_lock:{label:'GPS lock'},gps_wait:{label:'GPS waiting'}
};
const labelStyleDefs={
  aircraft:{label:'Aircraft labels',help:'Callsign row size; the flight-level/speed row is drawn two points smaller.'},
  aircraft_notification:{label:'Notification aircraft labels',help:'Used when an aircraft matches a notification. The flight-level/speed row is drawn two points smaller.',font:false},
  range_label:{label:'Range ring labels'},
  heading_label:{label:'Compass labels'},
  airport:{label:'Airport ICAO labels'},
  button:{label:'Radar button and menu labels'},
  notification:{label:'Notification banner text'},
  gps:{label:'GPS status text'}
};
const styleGroups=[
  {id:'screen',label:'Screen',keys:['screen_bg','radar_bg','radar_glow','portal_bg']},
  {id:'radar',label:'Radar Lines',keys:['radar_bright','radar_grid','radar_radial','radar_axis','sweep']},
  {id:'ticks',label:'Ticks',keys:['radar_tick_medium','radar_tick_minor']},
  {id:'map',label:'Map And Airports',keys:['country_boundary','airport','runway'],labelStyles:['airport']},
  {id:'controls',label:'Controls And Labels',keys:['control_arc','range_label','heading_label','button_text','button_status','button_pressed'],labelStyles:['range_label','heading_label','button']},
  {id:'aircraft',label:'Aircraft',keys:['aircraft_normal','aircraft_stale','aircraft_low','aircraft_emergency','aircraft_hit','climb_triangle','descent_triangle','aircraft_heading'],labelStyles:['aircraft','aircraft_notification']},
  {id:'altitude',label:'Altitude Colours',keys:['ground','below_2000','below_10000','below_20000','below_30000','below_40000','above_40000']},
  {id:'text',label:'Text And Popup',keys:['text_primary','text_secondary','popup_bg','popup_border'],labelStyles:['notification']},
  {id:'gps',label:'GPS',keys:['gps_neutral','gps_lock','gps_wait'],labelStyles:['gps']}
];
	// Escape text before inserting it into generated HTML fragments.
	function esc(s){return String(s||'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));}
	// Return true when latitude and longitude are usable map coordinates.
	function validLatLon(lat,lon){ lat=Number(lat); lon=Number(lon); return isFinite(lat)&&isFinite(lon)&&lat>=-90&&lat<=90&&lon>=-180&&lon<=180; }
	// Read the manual latitude and longitude fields as a map coordinate.
	function manualLatLng(){ let lat=+$('#lat').val(), lon=+$('#lon').val(); return validLatLon(lat,lon)?{lat:lat,lon:lon}:null; }
	// Get the location currently represented by the active centre source.
	function activeCentre(){
  let src=$('#centerSource').val();
  if(src==='gps'&&latestGps&&latestGps.hasFix) return {lat:+latestGps.lat,lon:+latestGps.lon,label:'USB GPS fix',help:latestGps.status||'Using current GPS lock'};
  if(src==='airport'&&selectedAirport) return {lat:+selectedAirport.lat,lon:+selectedAirport.lon,label:selectedAirport.code?`${selectedAirport.code} - ${selectedAirport.name}`:selectedAirport.name,help:'Airport database position'};
  if(src==='location'&&selectedLocation) return {lat:+selectedLocation.lat,lon:+selectedLocation.lon,label:locationLabel(selectedLocation),help:'Place search result'};
  let manual=manualLatLng();
  return manual?{lat:manual.lat,lon:manual.lon,label:'Manual position',help:'Click the map to move the manual radar centre'}:null;
}
	// Create the OpenStreetMap widget once, when the Location tab first needs it.
	function ensureLocationMap(){
  if(locationMap||typeof L==='undefined') return;
  locationMap=L.map('locationMap',{zoomControl:true,worldCopyJump:true}).setView([+$('#lat').val()||52.3579,+$('#lon').val()||0.1483],8);
  L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png',{maxZoom:18,attribution:'&copy; OpenStreetMap contributors'}).addTo(locationMap);
  locationMarker=L.marker(locationMap.getCenter(),{draggable:false}).addTo(locationMap);
  locationMap.on('click',function(e){
    if($('#centerSource').val()!=='manual'){ status('Select Manual to set the radar centre from the map','warning'); return; }
    setManualCentre(e.latlng.lat,e.latlng.lng,true);
  });
}
	// Update the map marker and readout from the active radar centre.
function refreshLocationMap(pan){
  if($('#general').hasClass('active')||locationMap) ensureLocationMap();
  let centre=activeCentre();
  let src=$('#centerSource').val();
  $('.location-source').removeClass('active');
  $(`.location-source[data-source="${src}"]`).addClass('active');
  $(`input[name="locationSourceCard"][value="${src}"]`).prop('checked',true);
  $('#mapTitle').text(centre?centre.label:'No valid centre selected');
  $('#mapHelp').text(centre?centre.help:'Enter coordinates or select a GPS, airport, or place result.');
  $('#mapCoords').text(centre?`${Number(centre.lat).toFixed(6)}, ${Number(centre.lon).toFixed(6)}`:'--');
  if(locationMap&&centre&&validLatLon(centre.lat,centre.lon)){
    let ll=[centre.lat,centre.lon];
    locationMarker.setLatLng(ll);
    if(pan!==false) locationMap.setView(ll, Math.max(locationMap.getZoom()||8, 8));
    setTimeout(()=>locationMap.invalidateSize(),80);
  }
}
	// Set the manual radar centre from map interaction or typed coordinates.
	function setManualCentre(lat,lon,pan){
  if(!validLatLon(lat,lon)) return;
  $('#lat').val(Number(lat).toFixed(6));
  $('#lon').val(Number(lon).toFixed(6));
  refreshLocationMap(pan);
}
	// Update the header badge with the current settings action state.
	function status(text, cls){ $('#saveStatus').removeClass().addClass('badge rounded-pill text-bg-'+(cls||'secondary')).text(text); }
	// Format a byte count for compact status cards and tables.
	function fmtBytes(v){ v=Number(v)||0; if(v<1024) return `${v} B`; if(v<1048576) return `${(v/1024).toFixed(1)} KB`; return `${(v/1048576).toFixed(2)} MB`; }
	// Format uptime seconds into a short human-readable duration.
	function fmtUptime(s){ s=Number(s)||0; let d=Math.floor(s/86400); s%=86400; let h=Math.floor(s/3600); s%=3600; let m=Math.floor(s/60); if(d) return `${d}d ${h}h ${m}m`; if(h) return `${h}h ${m}m`; return `${m}m ${Math.floor(s%60)}s`; }
	// Build one high-level device status metric card.
	function metricCard(label,value,detail){ return `<div class="col-sm-6 col-xl-3"><div class="metric-card"><div class="small-label">${esc(label)}</div><div class="metric-value">${esc(value)}</div><div class="text-muted small mt-1">${esc(detail||'')}</div></div></div>`; }
	// Build one compact key/value card for secondary status panels.
	function kvCard(label,value,detail){ return `<div class="col-sm-6"><div class="dashboard-mini-card"><div class="small-label">${esc(label)}</div><div class="fw-semibold">${esc(value)}</div><div class="text-muted small">${esc(detail||'')}</div></div></div>`; }
	// Render one heap capability row in the memory table.
	function memoryRow(name,h){ h=h||{}; let total=Number(h.total)||0, used=Number(h.used)||0, pct=total?Math.round((used/total)*100):0; return `<tr><th>${esc(name)}</th><td>${fmtBytes(used)} <span class="text-muted">(${pct}%)</span></td><td>${fmtBytes(h.free)}</td><td>${fmtBytes(h.largestFree)}</td><td>${fmtBytes(h.minFree)}</td></tr>`; }
	// Render one cache or buffer row in the status table.
	function cacheRow(c){ return `<tr><td>${esc(c.name)}</td><td><span class="badge rounded-pill text-bg-${c.active?'success':'secondary'}">${c.active?'Active':'Inactive'}</span></td><td>${fmtBytes(c.bytes)}</td></tr>`; }
	// Format aircraft altitude for the status aircraft table.
	function fmtAlt(v){ v=Number(v); if(!isFinite(v)||v<-1000000) return '--'; if(v===0) return 'GND'; return `${Math.round(v).toLocaleString()} ft`; }
	// Format a numeric value with a unit for status tables.
	function fmtUnit(v,unit,digits){ v=Number(v); if(!isFinite(v)) return '--'; return `${v.toFixed(digits||0)} ${unit}`; }
	// Return whether a notification rule already exists for an aircraft type.
	function notificationExistsForType(type){ type=(type||'').trim().toLowerCase(); if(!type) return true; return collectNotifications().some(n=>(n.type||'').trim().toLowerCase()===type); }
	// Add a notification rule for an aircraft type and persist it immediately.
	function addNotificationForType(type){ type=(type||'').trim(); if(!type){ status('Aircraft type missing','warning'); return; } if(notificationExistsForType(type)){ status(`Notification already exists for ${type}`,'warning'); return; } let rows=$('#notificationRows .notify-row'); let target=null; rows.each(function(){ if(!target&&!$('.notify-type',this).val().trim()) target=this; }); if(!target){ status('No free notification slots','warning'); return; } $('.notify-enabled',target).prop('checked',true); $('.notify-type',target).val(type.substring(0,15)); $('.notify-color',target).val('#ff0000'); $('.notify-text',target).val(`${type} Found`.substring(0,39)); saveSettings(); }
	// Render one aircraft row in the status aircraft table.
	function aircraftRow(a){ a=a||{}; let flags=[]; if(a.hasDbFlags&&((a.dbFlags||0)&1)) flags.push('MIL'); if(a.hasDbFlags&&((a.dbFlags||0)&2)) flags.push('INT'); let rawType=(a.type||'').trim(); let type=[rawType,flags.join(' ')].filter(Boolean).join(' / ')||'--'; let canAdd=rawType&&!notificationExistsForType(rawType); let action=canAdd?`<button class="btn btn-sm btn-outline-danger add-notification-type" type="button" data-type="${esc(rawType)}">Notify</button>`:`<span class="text-muted small">${rawType?'Added':'--'}</span>`; let rowClass=a.labelled?'aircraft-row-labelled':(a.onDisplay?'aircraft-row-displayed':''); let state=a.labelled?'<span class="badge text-bg-primary">Labelled</span>':(a.onDisplay?'<span class="badge text-bg-info">Plotted</span>':'<span class="badge text-bg-light text-muted">Off display</span>'); let displayGroup=a.onDisplay?'1 - On display':'2 - Not on display'; return `<tr class="${rowClass}"><th>${esc(a.callsign||'--')}<div class="text-muted small">${esc(a.registration||'')}</div></th><td>${displayGroup}</td><td>${state}</td><td>${esc(a.icao||'--')}</td><td>${esc(type)}</td><td>${fmtUnit(a.distanceMi,'MI',1)}</td><td>${fmtUnit(a.bearingDeg,'deg',0)}</td><td>${fmtAlt(a.altitudeFt)}</td><td>${fmtUnit(a.speedKt,'kt',0)}</td><td>${fmtUnit(a.seenS,'s',1)}</td><td class="text-end">${action}</td></tr>`; }
	// Render the aircraft list through DataTables, grouped by display state.
	function renderAircraftTable(items){
  let tableState={page:0,length:25,search:''};
  if(aircraftTable){
    tableState.page=aircraftTable.page();
    tableState.length=aircraftTable.page.len();
    tableState.search=aircraftTable.search();
    aircraftTable.destroy();
    aircraftTable=null;
  }
  let rows=(items||[]).map(aircraftRow).join('');
  $('#aircraftStatusRows').html(rows||'<tr><td colspan="11" class="text-muted">No aircraft currently available.</td></tr>');
  if(!rows || !$.fn.DataTable) return;
  aircraftTable=$('#aircraftStatusTable').DataTable({
    paging:true,
    pageLength:tableState.length,
    lengthMenu:[10,25,50,100,-1],
    order:[[1,'asc']],
    columnDefs:[
      {targets:1,visible:false,searchable:false},
      {targets:10,orderable:false,searchable:false}
    ],
    rowGroup:{
      dataSrc:1,
      startRender:function(rows,group){
        return `${String(group).replace(/^[0-9]+ - /,'')} (${rows.count()})`;
      }
    },
    language:{search:'Search aircraft:'}
  });
  if(tableState.search) aircraftTable.search(tableState.search);
  let pageInfo=aircraftTable.page.info();
  let targetPage=Math.min(tableState.page,Math.max(0,pageInfo.pages-1));
  aircraftTable.page(targetPage).draw(false);
}
	// Populate the status tab from the live diagnostics JSON response.
	function renderDeviceStatus(data){
  data=data||{}; let mem=data.memory||{}, wifi=data.wifi||{}, ac=data.aircraft||{}, gps=data.gps||{};
  let gpsPosition=gps.hasFix?`${Number(gps.lat).toFixed(6)}, ${Number(gps.lon).toFixed(6)}`:'No lock';
  $('#deviceStatusSummary').html([
    metricCard('Uptime',fmtUptime(data.uptimeSec),`IDF ${data.idfVersion||'unknown'}`),
    metricCard('Internal heap',fmtBytes(mem.internal&&mem.internal.free),`Largest ${fmtBytes(mem.internal&&mem.internal.largestFree)}`),
    metricCard('Aircraft data',`${ac.latestCount||0} targets`,`${ac.status||'WAIT'} / ${ac.filter||'All aircraft'}`),
    metricCard('WiFi',wifi.connected?'Connected':(wifi.portalActive?'Portal active':'Disconnected'),wifi.ip||'--')
  ].join(''));
  $('#deviceStatusTime').text(`Updated ${new Date().toLocaleTimeString()}`);
  $('#memoryStatusRows').html([
    memoryRow('Internal',mem.internal),
    memoryRow('PSRAM',mem.spiram),
    memoryRow('DMA',mem.dma),
    `<tr><th>Total free heap</th><td colspan="4">${fmtBytes(mem.freeHeap)} <span class="text-muted">minimum ${fmtBytes(mem.minFreeHeap)}</span></td></tr>`
  ].join(''));
  $('#cacheStatusRows').html((data.caches||[]).map(cacheRow).join('')||'<tr><td colspan="3" class="text-muted">No cache data.</td></tr>');
  $('#gpsStatusDetail').html([
    kvCard('GPS',gps.hasFix?'Fix':(gps.deviceConnected?'Waiting':'Not connected'),gps.status||'--'),
    kvCard('Position',gpsPosition,gps.hasFix?'Current GPS lock':'GPS position unavailable'),
    kvCard('Satellites',String(gps.satellites||0),`${gps.sentences||0} NMEA sentences`),
    kvCard('Detail',gps.detail||'--',gps.receiving?'Receiving GPS data':'No active GPS stream')
  ].join(''));
  $('#wifiStatusDetail').html([
    kvCard('Network',wifi.ssid||'Not configured',wifi.connected?'Station connected':(wifi.portalActive?'Captive portal active':'Not connected')),
    kvCard('IP address',wifi.ip||'--',wifi.started?'WiFi driver started':'WiFi driver stopped'),
    kvCard('Recovery',wifi.recovering?'Recovering':'Idle','HTTP fetches do not reset WiFi automatically'),
    kvCard('Tasks',String(data.taskCount||0),`${data.chipCores||0} cores, rev ${data.chipRevision||0}`)
  ].join(''));
  $('#aircraftStatusSummary').html([
    metricCard('Data source',ac.source||'--',ac.localUrl||ac.filter||''),
    metricCard('Targets',`${ac.latestCount||0}`,`${ac.uiCount||0} displayed`),
    metricCard('Range',`${ac.rangeMi||0} MI`,ac.status||'WAIT'),
    metricCard('Photos',ac.photoFetchRunning?'Fetching':'Idle',`Memory ${fmtBytes(ac.photoBytes)}`)
  ].join(''));
  $('#aircraftStatusTime').text(`Updated ${new Date().toLocaleTimeString()}`);
  renderAircraftTable(ac.items||[]);
}
	// Refresh the status tab from the ESP32 diagnostics endpoint.
	function loadDeviceStatus(){ $.getJSON('/api/status').done(renderDeviceStatus).fail(()=>$('#deviceStatusSummary').html('<div class="col-12"><div class="alert alert-danger mb-0">Status load failed</div></div>')); }
	// Load a fresh PNG capture from the current LVGL display.
	function setScreenshotLoading(active){
  $('#screenshotLoading').toggleClass('active',!!active);
  $('#refreshScreenshot').prop('disabled',!!active);
  $('#downloadScreenshot').toggleClass('disabled',!!active).attr('aria-disabled',active?'true':'false');
}
	function loadScreenshot(){
  let url='/api/screenshot.png?t='+Date.now();
  setScreenshotLoading(true);
  $('#screenshotStatus').text('Capturing display...');
  $('#downloadScreenshot').attr('href',url);
  $('#radarScreenshot').hide().off('load error')
    .on('load',function(){
      setScreenshotLoading(false);
      $('#screenshotStatus').text('Captured '+new Date().toLocaleTimeString());
      $(this).show();
    })
    .on('error',function(){
      setScreenshotLoading(false);
      $('#screenshotStatus').text('Screenshot capture failed');
    })
    .attr('src',url);
}
	// Clamp line widths to the range supported by the firmware settings parser.
	function clampWidth(v){ v=parseInt(v||1,10); return Math.max(1,Math.min(40,isNaN(v)?1:v)); }
	// Clamp a numeric UI setting before writing it back to the form.
	function clampRange(v,min,max){ v=parseInt(v||min,10); if(isNaN(v)) v=min; return Math.max(min,Math.min(max,v)); }
	// Test for an explicitly supplied property, including false and zero values.
	function hasKey(obj,key){ return !!obj && Object.prototype.hasOwnProperty.call(obj,key); }
	// Build the editor row for one configurable UI element.
function styleRow(key,data){
  let d=styleDefs[key], source=d.source||'colors', color=(data[source]&&data[source][key])||'#ffffff', width=clampWidth((data.widths&&data.widths[key])||1);
  let visible=hasKey(data.visible,key)?!!data.visible[key]:true;
  let colorHtml=d.color===false?'':`<div class="col-sm-4 col-md-3 col-xl-2"><label class="form-label">Colour</label><input class="form-control form-control-color color-value" type="color" data-key="${key}" data-source="${source}" value="${color}"></div>`;
  let visibleHtml=d.visible?`<div class="col-sm-3 col-md-2"><label class="form-label">Visible</label><div class="form-check form-switch mb-0"><input class="form-check-input visible-value" type="checkbox" data-key="${key}" ${visible?'checked':''}></div></div>`:'';
  let widthHtml=d.width?`<div class="col-sm-3 col-md-2"><label class="form-label">Width</label><input class="form-control width-control width-value" type="number" min="1" max="40" data-key="${key}" value="${width}"></div>`:'';
  let uiHtml=(d.ui||[]).map(u=>{ let value=clampRange(data.ui&&data.ui[u.key],u.min,u.max); return `<div class="col-sm-4 col-md-3 col-xl-2"><label class="form-label">${esc(u.label)}</label><input class="form-control ui-control ui-value" type="number" min="${u.min}" max="${u.max}" data-key="${u.key}" value="${value}"></div>`; }).join('');
  return `<div class="style-row"><div class="row g-3 align-items-end"><div class="col-sm-5 col-md-3 col-xl-2"><span class="small-label">${esc(d.label)}</span></div>${visibleHtml}${colorHtml}${widthHtml}${uiHtml}</div></div>`;
}
	// Build the editor row for one configurable radar label font.
function labelStyleRow(key,data){
  let d=labelStyleDefs[key], value=(data.labelStyles&&data.labelStyles[key])||{}, font=value.font||'montserrat', size=clampRange(value.size||12,10,22);
  let fontHtml=d.font===false?'':`<div class="col-sm-4 col-md-3 col-xl-2"><label class="form-label">Font</label><select class="form-select label-font-value" data-key="${key}"><option value="montserrat" ${font==='montserrat'?'selected':''}>Montserrat</option><option value="default" ${font==='default'?'selected':''}>LVGL default</option></select></div>`;
  return `<div class="style-row"><div class="row g-3 align-items-end"><div class="col-sm-5 col-md-3 col-xl-2"><span class="small-label">${esc(d.label)}</span>${d.help?`<div class="form-text">${esc(d.help)}</div>`:''}</div>${fontHtml}<div class="col-sm-3 col-md-2"><label class="form-label">Size</label><div class="input-group"><input class="form-control label-size-value" type="number" min="10" max="22" data-key="${key}" value="${size}"><span class="input-group-text">px</span></div></div></div></div>`;
}
	// Render the tabbed UI styling editor from the style group definitions.
function renderStyleGroups(data){
  let root=$('#styleGroups').empty();
  let tabs=$('<ul class="nav style-tabs flex-nowrap overflow-auto" id="styleGroupTabs" role="tablist"></ul>');
  let content=$('<div class="tab-content style-tab-content" id="styleGroupContent"></div>');
  tabs.append('<li class="nav-item" role="presentation"><button class="nav-link active" id="style-tab-display" data-bs-toggle="tab" data-bs-target="#displayPane" type="button" role="tab" aria-controls="displayPane" aria-selected="true">Display</button></li>');
  content.append(`<div class="tab-pane fade show active" id="displayPane" role="tabpanel" aria-labelledby="style-tab-display" tabindex="0">
    <div class="style-group">
      <div class="d-flex flex-wrap justify-content-between align-items-center gap-2 mb-3">
        <div><h2 class="h5 mb-1">Display</h2><div class="text-muted small">Radar layers, sweep behaviour, and aircraft movement indicators</div></div>
      </div>
      <div class="row g-3">
        <div class="col-xl-6">
          <div class="display-card">
            <div class="display-card-head"><div class="editor-card-icon"><i class="bi bi-radar"></i></div><div><h3>Sweep line</h3><p>Control the animated scan line and its CRT-style fading trail.</p></div></div>
            <div class="row g-3">
              <div class="col-12 form-check form-switch"><input id="showSweep" class="form-check-input" type="checkbox"><label class="form-check-label" for="showSweep">Display the sweep line</label></div>
              <div class="col-md-6"><label class="form-label" for="sweepStepDeg">Sweep step</label><div class="input-group"><input id="sweepStepDeg" class="form-control" type="number" min="1" max="30"><span class="input-group-text">deg</span></div><div class="form-text">Degrees advanced each redraw.</div></div>
              <div class="col-md-6"><label class="form-label" for="sweepDrawIntervalMs">Draw interval</label><div class="input-group"><input id="sweepDrawIntervalMs" class="form-control" type="number" min="10" max="250"><span class="input-group-text">ms</span></div><div class="form-text">Lower values use more CPU.</div></div>
              <div class="col-12 form-check form-switch"><input id="showSweepTrail" class="form-check-input" type="checkbox"><label class="form-check-label" for="showSweepTrail">Show fading trail</label></div>
              <div class="col-md-4"><label class="form-label" for="sweepTrailCount">Trail lines</label><input id="sweepTrailCount" class="form-control" type="number" min="0" max="50"></div>
              <div class="col-md-4"><label class="form-label" for="sweepTrailStepDeg">Trail step</label><div class="input-group"><input id="sweepTrailStepDeg" class="form-control" type="number" min="0.1" max="10" step="0.1"><span class="input-group-text">deg</span></div><div class="form-text">Spacing between faded lines.</div></div>
              <div class="col-md-4"><label class="form-label" for="sweepTrailWidth">Trail width</label><div class="input-group"><input id="sweepTrailWidth" class="form-control" type="number" min="1" max="40"><span class="input-group-text">px</span></div></div>
            </div>
          </div>
        </div>
        <div class="col-xl-6">
          <div class="display-card">
            <div class="display-card-head"><div class="editor-card-icon"><i class="bi bi-layers"></i></div><div><h3>Radar layers</h3><p>Choose which map and contextual layers are drawn on the radar face.</p></div></div>
            <div class="row g-3">
              <div class="col-md-6 form-check form-switch"><input id="showAirports" class="form-check-input" type="checkbox"><label class="form-check-label" for="showAirports">Show airports</label></div>
              <div class="col-md-6 form-check form-switch"><input id="showRunways" class="form-check-input" type="checkbox"><label class="form-check-label" for="showRunways">Show selected airport runways</label></div>
              <div class="col-md-6 form-check form-switch"><input id="showCountries" class="form-check-input" type="checkbox"><label class="form-check-label" for="showCountries">Show country outlines</label></div>
              <div class="col-md-6 form-check form-switch"><input id="emergencyRed" class="form-check-input" type="checkbox"><label class="form-check-label" for="emergencyRed">Emergency squawks in red</label></div>
            </div>
          </div>
        </div>
        <div class="col-xl-6">
          <div class="display-card">
            <div class="display-card-head"><div class="editor-card-icon"><i class="bi bi-signpost"></i></div><div><h3>Ground aircraft</h3><p>Control when aircraft with ground-level altitude should be plotted.</p></div></div>
            <div class="row g-3">
              <div class="col-12 form-check form-switch"><input id="showGroundAircraft" class="form-check-input" type="checkbox"><label class="form-check-label" for="showGroundAircraft">Show aircraft on the ground</label></div>
              <div class="col-md-7"><label class="form-label" for="groundSpeedKt">Ground speed threshold</label><div class="input-group"><input id="groundSpeedKt" class="form-control" type="number" min="0" max="250"><span class="input-group-text">kt</span></div><div class="form-text">Ground aircraft slower than this are hidden.</div></div>
            </div>
          </div>
        </div>
        <div class="col-xl-6">
          <div class="display-card">
            <div class="display-card-head"><div class="editor-card-icon"><i class="bi bi-send"></i></div><div><h3>Aircraft direction</h3><p>Control heading arrows and climb/descent indicators.</p></div></div>
            <div class="row g-3">
              <div class="col-md-7"><label class="form-label">Heading indicator</label><select id="headingMode" class="form-select"><option value="none">None</option><option value="arrow">Arrow</option><option value="line">Line</option></select></div>
              <div class="col-md-5 form-check form-switch align-self-end"><input id="showClimbDescent" class="form-check-input" type="checkbox"><label class="form-check-label" for="showClimbDescent">Show climb/descent</label></div>
            </div>
          </div>
        </div>
      </div>
    </div>
  </div>`);
  $('#showSweep',content).prop('checked',data.interface.showSweep);
  $('#showAirports',content).prop('checked',data.interface.showAirports);
  $('#showRunways',content).prop('checked',!!data.interface.showAirportRunways);
  $('#showCountries',content).prop('checked',data.interface.showCountries);
  $('#emergencyRed',content).prop('checked',data.interface.emergencyRed);
  $('#showGroundAircraft',content).prop('checked',!!data.interface.showGroundAircraft);
  $('#groundSpeedKt',content).val(data.interface.groundSpeedKt ?? 30);
  let headingMode=data.interface.headingMode||(data.interface.showHeading?'arrow':'none');
  if(headingMode==='heading'||headingMode==='vertical') headingMode='arrow';
  $('#headingMode',content).val(headingMode);
  $('#showClimbDescent',content).prop('checked',!!data.interface.showClimbDescent);
  $('#sweepStepDeg',content).val(data.interface.sweepStepDeg ?? 5);
  $('#sweepDrawIntervalMs',content).val(data.interface.sweepDrawIntervalMs ?? 50);
  $('#showSweepTrail',content).prop('checked',data.interface.showSweepTrail!==false);
  $('#sweepTrailCount',content).val(data.interface.sweepTrailCount ?? 5);
  $('#sweepTrailStepDeg',content).val(data.interface.sweepTrailStepDeg ?? 1);
  $('#sweepTrailWidth',content).val(data.interface.sweepTrailWidth ?? 6);
  styleGroups.forEach((g,i)=>{
    let active='';
    let selected='false';
    tabs.append(`<li class="nav-item" role="presentation"><button class="nav-link ${active}" id="style-tab-${g.id}" data-bs-toggle="tab" data-bs-target="#style-pane-${g.id}" type="button" role="tab" aria-controls="style-pane-${g.id}" aria-selected="${selected}">${esc(g.label)}</button></li>`);
    let pane=$(`<div class="tab-pane fade" id="style-pane-${g.id}" role="tabpanel" aria-labelledby="style-tab-${g.id}" tabindex="0"></div>`);
    let box=$(`<div class="style-group" data-group="${g.id}"><div class="d-flex flex-wrap justify-content-between align-items-center gap-2 mb-2"><h2 class="h5 mb-1">${esc(g.label)}</h2><button class="btn btn-sm btn-outline-secondary reset-style-group" type="button" data-group="${g.id}">Reset group</button></div></div>`);
    g.keys.forEach(k=>box.append(styleRow(k,data)));
    (g.labelStyles||[]).forEach(k=>box.append(labelStyleRow(k,data)));
    pane.append(box);
    content.append(pane);
  });
  root.append(tabs,content);
}
	// Build one range preset row.
	function rangeRow(i,r){ r=r||{}; return `<div class="range-row"><div class="editor-card-head"><div class="editor-card-title"><div class="editor-card-icon"><i class="bi bi-broadcast"></i></div><div><h3>Range ${i+1}</h3><p>${r.miles?`${r.miles} mile preset`:'Unused preset slot'}</p></div></div><span class="badge rounded-pill text-bg-light text-muted">Preset</span></div><div class="row g-3 align-items-end"><div class="col-md-4"><label class="form-label">Miles</label><div class="input-group"><input class="form-control range-miles" type="number" min="0" max="250" value="${r.miles||''}"><span class="input-group-text">MI</span></div></div><div class="col-md-4"><label class="form-label">Refresh</label><div class="input-group"><input class="form-control range-refresh" type="number" min="2" max="300" value="${r.refresh||10}"><span class="input-group-text">sec</span></div></div><div class="col-md-4"><label class="form-label">Labels</label><input class="form-control range-labels" type="number" min="0" max="50" value="${r.labels??24}"></div></div></div>`; }
	// Build one aircraft notification rule row.
	function notifyRow(i,n){ n=n||{}; let enabled=n.enabled!==false,bold=!!n.boldText,type=esc(n.type||''),text=esc(n.text||''); return `<div class="notify-row"><div class="editor-card-head"><div class="editor-card-title"><div class="editor-card-icon"><i class="bi bi-airplane"></i></div><div><h3>Notification ${i+1}</h3><p>${type?`Aircraft type contains "${type}"`:'Empty rule slot'}</p></div></div><button class="btn btn-sm btn-outline-danger delete-notification" type="button">Delete</button></div><div class="row g-3 align-items-end"><div class="col-sm-6 col-xl-3"><div class="form-check form-switch mb-0"><input class="form-check-input notify-enabled" type="checkbox" ${enabled?'checked':''}><label class="form-check-label">Enabled</label></div></div><div class="col-sm-6 col-xl-3"><div class="form-check form-switch mb-0"><input class="form-check-input notify-bold" type="checkbox" ${bold?'checked':''}><label class="form-check-label">Bold label</label></div></div><div class="col-md-6"><label class="form-label">Aircraft type contains</label><input class="form-control notify-type" maxlength="15" placeholder="A20N, B738, C130" value="${type}"></div><div class="col-md-6"><label class="form-label">Radar text</label><input class="form-control notify-text" maxlength="39" placeholder="Alert text shown on radar" value="${text}"></div><div class="col-md-4"><label class="form-label">Aircraft colour</label><input class="form-control form-control-color notify-color" type="color" value="${n.color||'#ffb020'}" title="Colour"></div></div></div>`; }
	// Rebuild the notification editor rows from a compacted notification list.
	function renderNotificationRows(items){ let rows=$('#notificationRows').empty(); items=items||[]; for(let i=0;i<10;i++) rows.append(notifyRow(i,items[i])); }
	// Create the display label used by the airport autocomplete and selection state.
	function airportLabel(a){ if(!a) return ''; let code=a.code?`${a.code} - `:''; return `${code}${a.name} (${Number(a.lat).toFixed(5)}, ${Number(a.lon).toFixed(5)})`; }
	// Update the selected airport and the related copy-to-manual control.
	function renderAirport(a){ selectedAirport=a&&a.name?{name:a.name,code:a.code||'',lat:+a.lat,lon:+a.lon}:null; $('#copyAirportToManual').prop('disabled',!selectedAirport); $('#airportDetail').text(selectedAirport?`Selected ${airportLabel(selectedAirport)}`:'Select an airport to use it as the radar centre.'); refreshLocationMap(); }
	// Create the display label used by the location search results.
	function locationLabel(l){ if(!l) return ''; let parts=[l.name]; if(l.state) parts.push(l.state); if(l.country) parts.push(l.country); return `${parts.join(', ')} (${Number(l.lat).toFixed(5)}, ${Number(l.lon).toFixed(5)})`; }
	// Update the selected location and the related copy-to-manual control.
	function renderLocation(l){ selectedLocation=l&&l.name?{name:l.name,state:l.state||'',country:l.country||'',lat:+l.lat,lon:+l.lon}:null; $('#copyLocationToManual').prop('disabled',!selectedLocation); $('#locationDetail').text(selectedLocation?`Selected ${locationLabel(selectedLocation)}`:'Search for a location to use it as the radar centre.'); refreshLocationMap(); }
	// Render OpenWeather geocoder results as selectable list items.
	function renderLocationResults(items){ locationResults=items||[]; let box=$('#locationResults').empty(); if(!locationResults.length){ box.append('<div class="list-group-item text-muted">No locations found</div>'); return; } locationResults.forEach((l,i)=>box.append(`<button class="list-group-item list-group-item-action location-result" type="button" data-index="${i}">${esc(locationLabel(l))}</button>`)); }
	// Refresh the GPS status panel from the latest settings JSON.
	function renderGps(gps){ gps=gps||{}; latestGps=gps; let cls=gps.usingFix?'success':(gps.deviceConnected?'warning':'secondary'); $('#gpsStatusBox').removeClass('alert-success alert-warning alert-secondary').addClass('alert-'+cls); $('#gpsStatus').text(gps.status||'GPS status unknown'); $('#gpsDetail').text(gps.detail||''); $('#copyGpsToManual').prop('disabled',!gps.hasFix); refreshLocationMap(false); }
	// Enable only the form controls that apply to the selected centre source.
	function updateSourceState(){ let src=$('#centerSource').val(); $('#lat,#lon').prop('disabled',src!=='manual'); $('#gpsStatusBox').toggle(src==='gps'); $('#airportBox').toggle(src==='airport'); $('#locationBox').toggle(src==='location'); $('#showRunways').prop('disabled',src!=='airport'); if(src!=='airport') $('#showRunways').prop('checked',false); refreshLocationMap(); }
	// Enable the local aircraft URL only when local receiver mode is selected.
	function updateDataSourceState(){ let src=$('#dataSource').val()||'airplanes_live'; $('.data-source-card').removeClass('active'); $(`.data-source-card[data-source="${src}"]`).addClass('active'); $(`input[name="dataSourceCard"][value="${src}"]`).prop('checked',true); $('#localAircraftUrl').prop('disabled',src!=='local'); $('#localSourcePanel').toggleClass('opacity-50',src!=='local'); }
	// Render the complete settings response into the page.
	function render(data){ cfg=data; $('#centerSource').val(data.general.centerSource||'manual'); $('#lat').val(data.general.lat); $('#lon').val(data.general.lon); $('#dataSource').val(data.general.dataSource||'airplanes_live'); $('#localAircraftUrl').val(data.general.localAircraftUrl||''); $('#airportDbToken').val((data.apiKeys&&data.apiKeys.airportDbToken)||''); $('#openWeatherApiKey').val((data.apiKeys&&data.apiKeys.openWeatherApiKey)||''); renderStyleGroups(data); $('#rangeRows').empty(); for(let i=0;i<10;i++) $('#rangeRows').append(rangeRow(i,data.ranges[i])); renderNotificationRows(data.notifications||[]); rebuildDefaultRange(data.general.defaultRange); $('#currentWifi').text(data.wifi.ssid||'Not configured'); let a=data.general.airport||{}; if(a.name){ $('#airportSearch').val(airportLabel(a)); renderAirport(a); } else { $('#airportSearch').val(''); renderAirport(null); } let l=data.general.location||{}; if(l.name){ $('#locationSearch').val(locationLabel(l)); renderLocation(l); } else { $('#locationSearch').val(''); renderLocation(null); } $('#locationResults').empty(); renderGps(data.gps); updateSourceState(); updateDataSourceState(); status('Loaded','success'); setTimeout(()=>refreshLocationMap(false),120); }
	// Rebuild the startup range dropdown from the current range preset rows.
	function rebuildDefaultRange(selected){ let sel=$('#defaultRange').empty(); collectRanges().forEach(r=>{ if(r.miles>0) sel.append(`<option value="${r.miles}">${r.miles} MI</option>`); }); sel.val(String(selected||(collectRanges()[0]||{}).miles||50)); }
	// Collect the range preset rows into the settings payload format.
	function collectRanges(){ let ranges=[]; $('#rangeRows .range-row').each(function(){ ranges.push({miles:+$('.range-miles',this).val()||0, refresh:+$('.range-refresh',this).val()||10, labels:+$('.range-labels',this).val()||0}); }); return ranges; }
	// Collect the aircraft notification rule rows.
	function collectNotifications(){ let items=[]; $('#notificationRows .notify-row').each(function(){ items.push({enabled:$('.notify-enabled',this).is(':checked'), boldText:$('.notify-bold',this).is(':checked'), type:$('.notify-type',this).val(), color:$('.notify-color',this).val(), text:$('.notify-text',this).val()}); }); return items; }
	// Collect configured element colours by firmware colour key.
	function collectColors(){ let colors={}; $('.color-value').each(function(){ if(($(this).data('source')||'colors')==='colors') colors[$(this).data('key')]=$(this).val(); }); return colors; }
	// Collect configured altitude band colours by firmware altitude key.
	function collectAltitudeColors(){ let colors={}; $('.color-value').each(function(){ if(($(this).data('source')||'colors')==='altitudeColors') colors[$(this).data('key')]=$(this).val(); }); return colors; }
	// Collect configured line widths by firmware element key.
	function collectWidths(){ let widths={}; $('.width-value').each(function(){ widths[$(this).data('key')]=clampWidth($(this).val()); }); return widths; }
	// Collect per-element visibility switches by firmware element key.
	function collectVisible(){ let visible={}; $('.visible-value').each(function(){ visible[$(this).data('key')]=$(this).is(':checked'); }); return visible; }
	// Collect geometry and spacing controls such as tick length and radial spacing.
	function collectUi(){ let ui={}; $('.ui-value').each(function(){ ui[$(this).data('key')]=+$(this).val()||0; }); return ui; }
	// Collect configured radar label fonts and sizes.
	function collectLabelStyles(){ let styles={}; $('.label-font-value').each(function(){ let key=$(this).data('key'); styles[key]=styles[key]||{}; styles[key].font=$(this).val(); }); $('.label-size-value').each(function(){ let key=$(this).data('key'); styles[key]=styles[key]||{}; styles[key].size=clampRange($(this).val(),10,22); }); return styles; }
	// Build the complete settings payload sent to the ESP32.
	function collectSettings(){ let airport=selectedAirport||(cfg&&cfg.general&&cfg.general.airport)||{}; let location=selectedLocation||(cfg&&cfg.general&&cfg.general.location)||{}; let general={centerSource:$('#centerSource').val(), lat:+$('#lat').val(), lon:+$('#lon').val(), defaultRange:+$('#defaultRange').val(), dataSource:$('#dataSource').val(), localAircraftUrl:$('#localAircraftUrl').val().trim(), airport:airport, location:location}; return {general:general, apiKeys:{airportDbToken:$('#airportDbToken').val().trim(), openWeatherApiKey:$('#openWeatherApiKey').val().trim()}, interface:{showSweep:$('#showSweep').is(':checked'), sweepStepDeg:+$('#sweepStepDeg').val()||5, sweepDrawIntervalMs:+$('#sweepDrawIntervalMs').val()||50, showSweepTrail:$('#showSweepTrail').is(':checked'), sweepTrailCount:+$('#sweepTrailCount').val()||0, sweepTrailStepDeg:+$('#sweepTrailStepDeg').val()||1, sweepTrailWidth:+$('#sweepTrailWidth').val()||6, showAirports:$('#showAirports').is(':checked'), showAirportRunways:$('#showRunways').is(':checked'), showCountries:$('#showCountries').is(':checked'), emergencyRed:$('#emergencyRed').is(':checked'), showGroundAircraft:$('#showGroundAircraft').is(':checked'), groundSpeedKt:+$('#groundSpeedKt').val()||0, headingMode:$('#headingMode').val(), showClimbDescent:$('#showClimbDescent').is(':checked')}, colors:collectColors(), altitudeColors:collectAltitudeColors(), widths:collectWidths(), visible:collectVisible(), ui:collectUi(), labelStyles:collectLabelStyles(), ranges:collectRanges(), notifications:collectNotifications()}; }
	// Load factory defaults so reset buttons can restore only selected groups.
	function loadDefaults(){ $.getJSON('/api/settings/defaults').done(data=>defaults=data); }
	// Load current settings and refresh the defaults cache.
	function loadSettings(){ $.getJSON('/api/settings').done(render).fail(()=>status('Load failed','danger')); loadDefaults(); }
	// Save the current form state and then re-render from the device response.
	function saveSettings(){ status('Saving','warning'); $.ajax({url:'/api/settings',method:'POST',data:JSON.stringify(collectSettings()),contentType:'application/json'}).done(()=>loadSettings()).fail(x=>status(x.responseText||'Save failed','danger')); }
	const mainTabByHash={dashboard:'#espStatus',status:'#espStatus',location:'#general','data-sources':'#apiKeys',display:'#colours',notifications:'#notifications',ranges:'#ranges',wifi:'#wifi'};
	const hashByMainTab={espStatus:'dashboard',general:'location',apiKeys:'data-sources',colours:'display',notifications:'notifications',ranges:'ranges',wifi:'wifi'};
	const statusTabByHash={'dashboard-info':'#statusInfo','dashboard-aircraft':'#statusAircraft','dashboard-screenshot':'#statusScreenshot','dashboard-import-export':'#statusImportExport','status-info':'#statusInfo','status-aircraft':'#statusAircraft','status-screenshot':'#statusScreenshot','status-import-export':'#statusImportExport'};
	const hashByStatusTab={statusInfo:'dashboard-info',statusAircraft:'dashboard-aircraft',statusScreenshot:'dashboard-screenshot',statusImportExport:'dashboard-import-export'};
	function importExportStatus(text,cls){ $('#importExportStatus').removeClass().addClass('alert alert-'+(cls||'secondary')+' py-2 mb-3').text(text); }
	function settingsExportDocument(){
  return {
    name:'ADSB Radar settings',
    version:SETTINGS_EXPORT_VERSION,
    exportedAt:new Date().toISOString(),
    settings:collectSettings()
  };
}
	function downloadText(filename,text,type){
  let blob=new Blob([text],{type:type||'application/json'});
  let url=URL.createObjectURL(blob);
  let a=document.createElement('a');
  a.href=url; a.download=filename; document.body.appendChild(a); a.click(); a.remove();
  setTimeout(()=>URL.revokeObjectURL(url),1000);
}
	function exportSettingsJson(){
  try{
    let doc=settingsExportDocument();
    let stamp=new Date().toISOString().replace(/[:.]/g,'-');
    downloadText(`adsb-radar-settings-${stamp}.json`,JSON.stringify(doc,null,2),'application/json');
    importExportStatus(`Exported settings version ${SETTINGS_EXPORT_VERSION}`,'success');
  }catch(e){
    importExportStatus('Export failed: '+e.message,'danger');
  }
}
	function normaliseImportedSettings(doc){
  if(!doc||typeof doc!=='object') throw new Error('The JSON root must be an object.');
  let version=null, settings=null;
  if(Object.prototype.hasOwnProperty.call(doc,'settings')){
    version=doc.version;
    settings=doc.settings;
  } else {
    settings=doc;
  }
  if(!settings||typeof settings!=='object') throw new Error('The import does not contain a settings object.');
  return {version:version,settings:settings};
}
	function importSettingsJson(){
  let text=$('#importSettingsText').val().trim();
  if(!text){ importExportStatus('Choose a JSON file or paste JSON first.','warning'); return; }
  let parsed;
  try{
    parsed=normaliseImportedSettings(JSON.parse(text));
  }catch(e){
    importExportStatus('Import JSON is invalid: '+e.message,'danger');
    return;
  }
  if(parsed.version!==null && Number(parsed.version)!==SETTINGS_EXPORT_VERSION){
    if(!confirm(`This export is version ${parsed.version}; this firmware expects version ${SETTINGS_EXPORT_VERSION}. Import anyway?`)){
      importExportStatus('Import cancelled after version warning.','warning');
      return;
    }
  } else if(parsed.version===null) {
    if(!confirm(`This JSON has no export version. Import it as a raw settings object?`)){
      importExportStatus('Import cancelled.','warning');
      return;
    }
  }
  importExportStatus('Importing settings...','warning');
  $.ajax({url:'/api/settings',method:'POST',contentType:'application/json',data:JSON.stringify(parsed.settings)})
    .done(()=>{ importExportStatus('Settings imported and applied.','success'); loadSettings(); })
    .fail(x=>importExportStatus(x.responseText||'Import failed','danger'));
}
	// Update the address bar so a refresh returns to the currently selected admin page.
	function setPageHash(hash){ if(!hash) return; let target='#'+hash; if(window.location.hash===target) return; history.replaceState(null,'',window.location.pathname+window.location.search+target); }
	// Show one Bootstrap tab button by its target pane id.
	function showTabForTarget(target){ let btn=$(`button[data-bs-target="${target}"]`)[0]; if(btn) bootstrap.Tab.getOrCreateInstance(btn).show(); }
	// Restore the selected settings page from the URL fragment.
	function activateTabFromUrl(){
  let hash=(window.location.hash||'').replace(/^#/,'');
  if(statusTabByHash[hash]){
    showTabForTarget('#espStatus');
    showTabForTarget(statusTabByHash[hash]);
    return;
  }
  showTabForTarget(mainTabByHash[hash]||'#espStatus');
}
	// Apply default colour, width, visibility, and geometry values for selected keys.
	function applyDefaults(keys){
  if(!defaults){ status('Defaults not loaded','warning'); return false; }
  keys.forEach(k=>{
    if(defaults.colors&&defaults.colors[k]) $(`.color-value[data-key="${k}"][data-source="colors"]`).val(defaults.colors[k]);
    if(defaults.altitudeColors&&defaults.altitudeColors[k]) $(`.color-value[data-key="${k}"][data-source="altitudeColors"]`).val(defaults.altitudeColors[k]);
    if(defaults.widths&&defaults.widths[k]) $(`.width-value[data-key="${k}"]`).val(defaults.widths[k]);
    if(defaults.visible&&hasKey(defaults.visible,k)) $(`.visible-value[data-key="${k}"]`).prop('checked',!!defaults.visible[k]);
    (styleDefs[k].ui||[]).forEach(u=>{ if(defaults.ui&&hasKey(defaults.ui,u.key)) $(`.ui-value[data-key="${u.key}"]`).val(defaults.ui[u.key]); });
  });
  return true;
}
function applyLabelStyleDefaults(keys){
  if(!defaults){ status('Defaults not loaded','warning'); return false; }
  (keys||[]).forEach(k=>{
    if(defaults.labelStyles&&defaults.labelStyles[k]){
      $(`.label-font-value[data-key="${k}"]`).val(defaults.labelStyles[k].font||'montserrat');
      $(`.label-size-value[data-key="${k}"]`).val(defaults.labelStyles[k].size||12);
    }
  });
  return true;
}
$('.save-settings').on('click',saveSettings);
$('#centerSource').on('change',updateSourceState);
$('input[name="locationSourceCard"]').on('change',function(){ $('#centerSource').val($(this).val()); updateSourceState(); });
$('.location-source').on('click',function(){ $('#centerSource').val($(this).data('source')); updateSourceState(); });
$('#lat,#lon').on('input',function(){ if($('#centerSource').val()==='manual') refreshLocationMap(false); });
$('#dataSource').on('change',updateDataSourceState);
$('input[name="dataSourceCard"]').on('change',function(){ $('#dataSource').val($(this).val()); updateDataSourceState(); });
$('.data-source-card').on('click',function(){ $('#dataSource').val($(this).data('source')); updateDataSourceState(); });
$('#copyGpsToManual').on('click',function(){ $.getJSON('/api/settings').done(data=>{ if(!data.gps||!data.gps.hasFix){ status('No GPS lock','warning'); return; } $('#lat').val(Number(data.gps.lat).toFixed(6)); $('#lon').val(Number(data.gps.lon).toFixed(6)); $('#centerSource').val('manual'); updateSourceState(); saveSettings(); }).fail(()=>status('GPS copy failed','danger')); });
$('#airportSearch').on('input',function(){ let text=$(this).val(); if(airportOptions[text]){ renderAirport(airportOptions[text]); return; } renderAirport(null); clearTimeout(airportTimer); if(text.length<2) return; airportTimer=setTimeout(()=>{ $.getJSON('/api/airports',{q:text}).done(data=>{ airportOptions={}; let list=$('#airportOptions').empty(); (data.airports||[]).forEach(a=>{ let label=airportLabel(a); airportOptions[label]=a; list.append(`<option value="${esc(label)}"></option>`); }); if(airportOptions[$('#airportSearch').val()]) renderAirport(airportOptions[$('#airportSearch').val()]); }); },250); });
$('#copyAirportToManual').on('click',function(){ if(!selectedAirport){ status('No airport selected','warning'); return; } $('#lat').val(Number(selectedAirport.lat).toFixed(6)); $('#lon').val(Number(selectedAirport.lon).toFixed(6)); $('#centerSource').val('manual'); updateSourceState(); saveSettings(); });
	// Query OpenWeather for a place name and render selectable radar centre results.
	function searchLocations(){ let text=$('#locationSearch').val().trim(); if(text.length<2){ status('Enter a location','warning'); return; } status('Searching location','warning'); $.getJSON('/api/locations',{q:text}).done(data=>{ renderLocationResults(data.locations||[]); status('Location search complete','success'); }).fail(x=>status(x.responseText||'Location search failed','danger')); }
$('#searchLocation').on('click',searchLocations);
$('#locationSearch').on('keydown',function(e){ if(e.key==='Enter'){ e.preventDefault(); searchLocations(); } });
$('#locationResults').on('click','.location-result',function(){ let item=locationResults[+$(this).data('index')]; if(!item) return; renderLocation(item); $('#centerSource').val('location'); updateSourceState(); });
$('#copyLocationToManual').on('click',function(){ if(!selectedLocation){ status('No location selected','warning'); return; } $('#lat').val(Number(selectedLocation.lat).toFixed(6)); $('#lon').val(Number(selectedLocation.lon).toFixed(6)); $('#centerSource').val('manual'); updateSourceState(); saveSettings(); });
$('#styleGroups').on('click','.reset-style-group',function(){ let group=styleGroups.find(g=>g.id===$(this).data('group')); if(group&&applyDefaults(group.keys)&&applyLabelStyleDefaults(group.labelStyles||[])) saveSettings(); });
$('#resetAppearance').on('click',function(){ let keys=[], labelKeys=[]; styleGroups.forEach(g=>{ keys=keys.concat(g.keys); labelKeys=labelKeys.concat(g.labelStyles||[]); }); if(applyDefaults(keys)&&applyLabelStyleDefaults(labelKeys)) saveSettings(); });
$('#notificationRows').on('click','.delete-notification',function(){ let index=$('#notificationRows .notify-row').index($(this).closest('.notify-row')); let items=collectNotifications(); items.splice(index,1); items=items.filter(n=>(n.type||'').trim()||(n.text||'').trim()); renderNotificationRows(items); saveSettings(); });
$('#rangeRows').on('input','.range-miles',()=>rebuildDefaultRange($('#defaultRange').val()));
$('#resetRanges').on('click',()=>$.post('/api/ranges/reset').done(loadSettings).fail(()=>status('Reset failed','danger')));
$('#scanWifi').on('click',function(){ status('Scanning','warning'); $.getJSON('/api/wifi/scan').done(data=>{ let sel=$('#wifiSsid').empty(); data.networks.forEach(n=>sel.append(`<option value="${esc(n.ssid)}">${esc(n.ssid)} (${n.rssi} dBm)</option>`)); status('Scan complete','success'); }).fail(()=>status('Scan failed','danger')); });
$('#saveWifi').on('click',function(){ let ssid=$('#wifiManual').val()||$('#wifiSsid').val(); $.ajax({url:'/api/wifi',method:'POST',contentType:'application/json',data:JSON.stringify({ssid:ssid,password:$('#wifiPassword').val()})}).done(()=>status('WiFi saved','success')).fail(x=>status(x.responseText||'WiFi save failed','danger')); });
$('#exportSettings').on('click',exportSettingsJson);
$('#importSettings').on('click',importSettingsJson);
$('#importSettingsFile').on('change',function(){
  let file=this.files&&this.files[0];
  if(!file) return;
  let reader=new FileReader();
  reader.onload=function(){ $('#importSettingsText').val(String(reader.result||'')); importExportStatus(`Loaded ${file.name}`,'info'); };
  reader.onerror=function(){ importExportStatus('Could not read '+file.name,'danger'); };
  reader.readAsText(file);
});
$('#aircraftStatusRows').on('click','.add-notification-type',function(){ addNotificationForType($(this).data('type')); });
$('#refreshDeviceStatus').on('click',loadDeviceStatus);
$('button[data-bs-toggle="tab"]').on('shown.bs.tab',function(e){
  let id=$($(e.target).data('bs-target')).attr('id');
  if(hashByStatusTab[id]) setPageHash(hashByStatusTab[id]);
  else if(hashByMainTab[id]) setPageHash(hashByMainTab[id]);
  if(id==='general') setTimeout(()=>refreshLocationMap(false),120);
});
$('button[data-bs-target="#statusScreenshot"]').on('shown.bs.tab',loadScreenshot);
$('#refreshScreenshot').on('click',loadScreenshot);
$('button[data-bs-target="#espStatus"]').on('shown.bs.tab',loadDeviceStatus);
$(function(){ activateTabFromUrl(); loadSettings(); if($('#espStatus').hasClass('active')) loadDeviceStatus(); });
$(window).on('hashchange',activateTabFromUrl);
setInterval(()=>{ if(cfg&&$('#centerSource').val()==='gps') $.getJSON('/api/settings').done(data=>renderGps(data.gps)); },5000);
setInterval(()=>{ if($('#espStatus').hasClass('active')) loadDeviceStatus(); },5000);
</script>
</body>
</html>)HTML";
