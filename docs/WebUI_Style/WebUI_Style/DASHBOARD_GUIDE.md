# 🏭 Industrial Edge Gateway Dashboard System
## Multi-Page Implementation Guide

**Version**: 3.0 (Multi-Page Edition)  
**Date**: April 2, 2026  
**Status**: ✅ Production Ready

---

## 📋 Table of Contents

1. [System Overview](#system-overview)
2. [File Structure](#file-structure)
3. [Page Architecture](#page-architecture)
4. [UI Components](#ui-components)
5. [Theme System](#theme-system)
6. [Quick Start](#quick-start)
7. [Integration Guide](#integration-guide)

---

## 🎯 System Overview

This is a **complete multi-page industrial dashboard system** built on the v2.0 component foundation with 4 main pages:

| Page | Purpose | Features |
|------|---------|----------|
| **index.html** | Home/Landing | Navigation hub, component showcase, theme toggle |
| **monitor.html** | Real-time Dashboard | Live data cards, status indicators, refresh controls |
| **config.html** | Protocol Setup | Modbus, OPC UA, MQTT, Network configuration with tabs |
| **mapping.html** | Data Workflow | Map data sources to UI, preview generated components |

---

## 📁 File Structure

```
WebUI_Style/
├── index.html                    # Home/landing + component showcase
├── monitor.html                  # Real-time monitoring dashboard
├── config.html                   # Multi-protocol configuration
├── mapping.html                  # Data source mapping workflow
├── styles.css                    # Design system + themes
├── components.css                # All component styles
├── scripts.js                    # Shared interactions
├── README.md                     # Original documentation
├── QUICKSTART.md                 # Quick reference
├── IMPLEMENTATION_SUMMARY.md     # v1.0 details
├── VERSION_2_SUMMARY.md          # v2.0 features
├── CHANGELOG_V2.md               # v2.0 changelog
└── DASHBOARD_GUIDE.md            # This file (v3.0 architecture)
```

---

## 🏗️ Page Architecture

### 1️⃣ **index.html** - Home/Landing Page

**Purpose**: Showcase all components and provide navigation to dashboard

**Key Sections**:
- 🎨 Header with v2.0 branding
- 🔘 Quick access buttons (Monitor, Config, Mapping)
- 📝 Form components demo (checkbox, input, button)
- 📊 Data display demo (table, cards, LED)
- 🧭 Navigation components demo (tabs, glow)
- 🏭 Industrial components showcase
- 📐 Size variants comparison
- 🎨 Design system reference
- 🌓 Dark/Light theme toggle

**Layout**:
```
┌─────────────────────────────────┐
│         Demo Header             │
│   Quick Access Buttons (3)      │
├─────────────────────────────────┤
│ Size Control | Theme Toggle     │
├─────────────────────────────────┤
│  Form Comp | Data Display       │
│  Navigation | Industrial        │
│  Size Variants | Design System  │
├─────────────────────────────────┤
│          Footer Stats           │
└─────────────────────────────────┘
```

### 2️⃣ **monitor.html** - Real-Time Dashboard

**Purpose**: Display live monitoring data from configured sensors

**Layout**:
```
┌──────────────────────────────────────────────────┐
│ Sidebar Nav | Top Bar (Title + Refresh)          │
├──────────────┼──────────────────────────────────┤
│              │ Stats Grid (4 boxes)              │
│   Nav Menu   │ ────────────────────────────────│
│              │ Action Buttons (Start/Stop/Export)
│  • Home      │ ────────────────────────────────│
│  • Monitor   │ Monitor Cards Grid (responsive)  │
│  • Config    │ • Temperature | Humidity         │
│  • Mapping   │ • Pressure | CPU | Memory        │
│  • Theme     │ • Disk | Network | Errors        │
│              │                                  │
└──────────────┴──────────────────────────────────┘
```

**Features**:
- 📊 Real-time monitoring cards
- 🔄 Auto-refresh with timestamp
- 📈 Status indicators (green/yellow/red)
- 🎛️ Control buttons (Start/Stop/Export)
- 📱 Responsive grid layout (auto-fit)
- 🌓 Dark/Light mode support
- 🎨 Status colors with glowing animation

**Monitor Card Structure**:
```
┌─────────────────────────┐
│ Temperature Sensor 1    │  <- Header (label)
│ 24.5                    │  <- Value (large)
│ °C                      │  <- Unit
│ ✓ Normal               │  <- Status badge
└─────────────────────────┘
       (Green glow border)
```

### 3️⃣ **config.html** - Protocol Configuration

**Purpose**: Configure connections to data sources (PLC, OPC UA, MQTT, Network)

**Layout**:
```
┌──────────────────────────────────────────────────┐
│ Sidebar Nav | Top Bar (Title + Save Button)      │
├──────────────┼──────────────────────────────────┤
│              │ Protocol Tabs:                    │
│   Nav Menu   │ [📡 PLC] [🔗 OPC UA] [🌐 MQTT]   │
│              │ [🌍 Network]                     │
│              │ ────────────────────────────────│
│  • Home      │ Form Section (Dynamic per tab):  │
│  • Monitor   │ • Input fields (serial, endpoint)│
│  • Config    │ • Select dropdowns               │
│  • Mapping   │ • Parameter table                │
│  • Theme     │ • Action buttons                 │
│              │                                  │
└──────────────┴──────────────────────────────────┘
```

**Tab Sections**:

**📡 PLC Protocol Tab**:
- Modbus RTU/TCP configuration
- Port selection (/dev/ttyUSB0)
- Baud rate, data bits, stop bits
- Registered devices table
- Add/Edit/Remove device buttons
- Test connection functionality

**🔗 OPC UA Tab**:
- Client/Server mode selection
- Server endpoint URL
- Security policy selection
- Authentication (username/password)
- Subscribed nodes table
- Browse nodes button
- Test connection

**🌐 MQTT Tab**:
- Broker address and port
- Client ID, username, password
- QoS level selection
- MQTT topics table
- Add topic button
- Test connection

**🌍 Network Tab**:
- IP address configuration
- Subnet mask, gateway, DNS servers
- Apply/Reset buttons

### 4️⃣ **mapping.html** - Data Mapping Workflow

**Purpose**: Map external data sources to internal variables and UI components

**Workflow**:
```
Step 1: Define Value Name
  "Temperature" ──────→

                       Step 2: Select Data Source
                        "Modbus RTU" ──────→

                                            Step 3: Choose Variable
                                             "$temp" ──────→

                                                            Step 4: Select UI Type
                                                             "Monitor Card" ──────→

Result: New card in Monitor dashboard with auto-generated code
```

**Layout**:
```
┌──────────────────────────────────────────────────┐
│ Sidebar Nav | Top Bar (Title + Save Button)      │
├──────────────┼──────────────────────────────────┤
│              │ Info Box (How It Works)           │
│   Nav Menu   │ ────────────────────────────────│
│              │ Workflow Steps (4 boxes):        │
│  • Home      │ ┌──────────────────────────────┐│
│  • Monitor   │ │ 1️⃣ Value Name (text input) ││
│  • Config    │ │ 2️⃣ Data Source (select)    ││
│  • Mapping   │ │ 3️⃣ Internal Var (select)   ││
│  • Theme     │ │ 4️⃣ UI Type (select)        ││
│              │ └──────────────────────────────┘│
│              │ [✨ Create] [👁️ Preview]       │
│              │ ────────────────────────────────│
│              │ Active Mappings Table           │
│              │ (Edit/Delete functionality)     │
│              │ ────────────────────────────────│
│              │ Generated Preview Cards        │
│              │                                  │
└──────────────┴──────────────────────────────────┘
```

**Mapping Table Columns**:
| Column | Purpose |
|--------|---------|
| Display Name | User-friendly label |
| Data Source | Origin (PLC/OPC/MQTT) |
| Internal Variable | `$variable_name` |
| UI Type | Card/Input/Toggle/Button |
| Address/Topic | Modbus reg, OPC node ID, MQTT topic |
| Status | Active/Inactive badge |
| Actions | Edit/Delete buttons |

---

## 🎨 UI Components

### Sidebar Navigation
- Fixed left panel on desktop (250px width)
- Responsive mobile (top horizontal nav)
- Logo at top
- Active state highlighting
- Theme toggle at bottom

### Top Bar
- Page title
- Control buttons (Refresh, Save, etc.)
- Status information
- Responsive: stacks on mobile

### Monitor Cards
- 3 size variants (sm/md/lg)
- Status colors: green/yellow/red
- Glowing border left side
- Value display (monospace font)
- Status badge with animated indicator
- Hover effects (lift, shadow increase)

### Forms
- Text inputs (with inner shadow)
- Select dropdowns
- Labels (uppercase, small)
- Focus states (green border + glow)
- 100% width in containers

### Tables
- Header row highlighted
- Hover row highlight
- Monospace font for values
- Status badges with colors
- Responsive scroll on mobile

### Buttons
- 3 variants: primary (green), secondary (gray), danger (red)
- Raised effect (outer shadow)
- Glow on hover
- Scale animation on hover (1.05x)
- Responsive: full width on mobile

---

## 🌓 Theme System

**How It Works**:

```css
/* Default Dark Theme */
:root {
  --color-bg-primary: #0f1117;
  --color-text-primary: #e8eef2;
  --color-accent-green: #00ffcc;
  ...
}

/* Light Theme Overrides */
[data-theme="light"] {
  --color-bg-primary: #f5f7fa;
  --color-text-primary: #1a1a1a;
  --color-accent-green: #27ae60;
  ...
}
```

**Theme Toggle**:
```javascript
// Toggle theme
document.documentElement.setAttribute('data-theme', 'light');
localStorage.setItem('theme', 'light');

// Load saved preference
const savedTheme = localStorage.getItem('theme') || 'dark';
setTheme(savedTheme);
```

**Available Themes**:
- 🌙 **Dark** (Default) - High contrast, tech aesthetic
- ☀️ **Light** - Professional, office environment

---

## ⚡ Quick Start

### 1. Open Home Page
```bash
open index.html
```
Displays v2.0 component showcase + navigation to business pages

### 2. Navigate to Monitor
```
Click "📊 Monitor Dashboard" or use sidebar
```
See real-time data from configured sensors

### 3. Configure Protocols
```
Click "⚙️ Configuration" → Select tab (PLC/OPC UA/MQTT)
```
Set up data source connections

### 4. Map Data to UI
```
Click "🔗 Data Mapping" → Follow 4-step workflow
```
Create monitor cards from data sources

### 5. Toggle Theme
```
Click moon/sun icon in sidebar (all pages)
```
Switch between dark/light modes (persistent)

---

## 🔌 Integration Guide

### Adding New Data Sources

**Step 1: Add to Config**
- Go to `config.html` → appropriate protocol tab
- Add device/endpoint details
- Save configuration

**Step 2: Create Mapping**
- Go to `mapping.html`
- Define value name, select source, choose variable, select UI type
- Click "Create Mapping"

**Step 3: View in Monitor**
- Go to `monitor.html`
- New card appears in grid
- Real-time data updates

### Custom Components

**For custom monitor cards**, edit `monitor.html` and add:
```html
<div class="monitor-card status-green">
  <div class="monitor-card-header">Custom Sensor</div>
  <div class="monitor-card-value" id="custom-value">--</div>
  <div class="monitor-card-unit">units</div>
  <div class="monitor-card-status">
    <span class="status-indicator green"></span>
    <span>Status</span>
  </div>
</div>
```

Update with JavaScript:
```javascript
document.getElementById('custom-value').textContent = newValue;
```

### Data Binding

**For real-time updates from backend**:
```javascript
// Poll data source
setInterval(() => {
  fetch('/api/sensor-data')
    .then(r => r.json())
    .then(data => {
      document.getElementById('temp-value').textContent = data.temperature;
    });
}, 1000); // Update every 1 second
```

---

## 📊 Component Reference

### Monitor Card
```html
<div class="monitor-card status-green">
  <div class="monitor-card-header">Label</div>
  <div class="monitor-card-value">Value</div>
  <div class="monitor-card-unit">Unit</div>
  <div class="monitor-card-status">
    <span class="status-indicator green"></span>
    Status
  </div>
</div>
```

**Classes**:
- `.monitor-card` - Container
- `.status-green / .status-yellow / .status-red` - Border color
- `.status-indicator` - Animated glow dot
- `.status-indicator.green / .yellow / .red` - Color variants

### Form Input
```html
<input type="text" class="form-input" placeholder="Enter value" />
```

### Form Select
```html
<select class="form-select">
  <option>Option 1</option>
  <option>Option 2</option>
</select>
```

### Button
```html
<button class="action-btn">Click Me</button>
<button class="action-btn secondary">Secondary</button>
<button class="action-btn danger">Danger</button>
```

---

## 🚀 Deployment

### Requirements
- Modern browser (Chrome, Firefox, Safari, Edge)
- JavaScript enabled
- No backend required for demo (standalone HTML files)

### For Server Deployment
```bash
# Copy all files to web server
cp -r WebUI_Style/* /var/www/html/dashboard/

# Server should support:
# - Static file serving (HTML, CSS, JS)
# - localStorage API
# - ES6 JavaScript
```

### Optional: Backend Integration
```javascript
// Replace with actual API endpoints
const API_BASE = 'https://your-backend.com/api';

fetch(`${API_BASE}/sensor-data`)
  .then(r => r.json())
  .then(data => updateDashboard(data));
```

---

## 🎯 Key Features Summary

✅ **4 Professional Pages**
- Home/Landing with navigation
- Real-time monitoring dashboard
- Multi-tab protocol configuration
- Data mapping workflow with preview

✅ **Industrial Grade UI**
- Dark/Light themes (persistent)
- 13 reusable components
- 3 size variants
- 3D neumorphism design

✅ **Full Responsiveness**
- Desktop (1024px+) - 3-column layout
- Tablet (768px) - Sidebar → top nav
- Mobile (320px) - Stacked layout

✅ **Production Ready**
- Zero dependencies
- No build tools required
- Vanilla HTML/CSS/JavaScript
- Cross-browser compatible

✅ **Extensible Architecture**
- Easy to add protocols
- Template-based components
- CSS custom properties for customization
- localStorage for preferences

---

## 📝 Customization

### Change Accent Color
Edit `styles.css`:
```css
:root {
  --color-accent-green: #00ffcc; /* Change to your color */
}
```

### Add New Protocol Tab
In `config.html`:
```html
<button class="config-tab" onclick="switchTab(event, 'newprotocol')">
  🔌 New Protocol
</button>
<div id="newprotocol" class="tab-content">
  <!-- Your form here -->
</div>
```

### Customize Monitor Cards
Edit CSS in `monitor.html` `<style>` section:
```css
.monitor-card {
  grid-column: span 2; /* Make wider */
  min-height: 200px; /* Make taller */
}
```

---

## 🛠️ Troubleshooting

| Issue | Solution |
|-------|----------|
| Theme not persisting | Check localStorage enabled in browser |
| Cards not responsive | Clear browser cache, reload |
| Buttons not clicking | Ensure JavaScript not disabled |
| Sidebar too narrow on mobile | Use Chrome DevTools device emulation |
| Forms not submitting | Add backend endpoint, check console |

---

## 📞 Support & Documentation

- **v1.0 Details**: See `IMPLEMENTATION_SUMMARY.md`
- **v2.0 Features**: See `VERSION_2_SUMMARY.md`  
- **v2.0 Changelog**: See `CHANGELOG_V2.md`
- **Quick Reference**: See `QUICKSTART.md`
- **Full API**: See `README.md`

---

**Version 3.0 © 2026 - Industrial Edge Gateway Dashboard System**  
**Status**: ✅ Production Ready | **Last Updated**: April 2, 2026
