# 💎 Monitor Card Value Component - Premium Display Enhancement

**Version**: v2.0+ (New Component Addition)  
**Date**: April 2, 2026  
**Component 14/14**: Premium data display with glow borders & animated status  
**Status**: ✅ Production Ready

---

## 📋 Overview

The **Monitor Card Value** is a premium data display component inspired by the chart card aesthetic. It features sophisticated outer glow borders, soft gradients, recessed surfaces, and animated center-spread status indicators.

### Position in Component Library
- Components 1-13: Standard industrial components (form, data, navigation, industrial)
- **Component 14**: Premium display component (new)

---

## 🎨 Visual Design

### Outer Glow Border Effect
- Soft radial glow emanating from card edges
- Inspired by chart card floating aesthetic
- Subtle gradient borders (1px solid with transparency)
- Dark/light mode adaptive colors

```css
box-shadow:
  var(--shadow-outer-md),
  inset 0 0 0 1px rgba(0, 255, 204, 0.2),
  0 0 20px rgba(0, 255, 204, 0.2);
```

### Status Bar Animation
- Bottom 4px bar with center-spread glow
- Animated pulse effect (2s cycle, infinite)
- Center gradient: transparent → strong → transparent
- Colors: Green (normal) | Yellow (warning) | Red (critical)

```css
@keyframes glow-center-spread {
  0%, 100% { opacity: 0.6; filter: blur(0px); }
  50% { opacity: 1; filter: blur(2px); }
}
```

### Recessed Surface
- Dark/light mode adaptive background
- Deep inner shadow for depth
- Floating lift effect on hover (translateY -4px)
- Enhanced glow on hover

---

## 📐 Component Structure

```html
<div class="monitor-card-value size-md status-green">
  <!-- Header: Value name/label -->
  <div class="monitor-card-value-header">Temperature</div>
  
  <!-- Body: Value + Unit -->
  <div class="monitor-card-value-body">
    <div class="monitor-card-value-value">24.5</div>
    <div class="monitor-card-value-unit">°C</div>
  </div>
  
  <!-- Footer: Animated status bar -->
  <div class="monitor-card-value-footer"></div>
</div>
```

### Class Structure
| Class | Purpose |
|-------|---------|
| `.monitor-card-value` | Root container |
| `.monitor-card-value-header` | Label/name |
| `.monitor-card-value-body` | Value container (flex column) |
| `.monitor-card-value-value` | Main value display |
| `.monitor-card-value-unit` | Unit text |
| `.monitor-card-value-footer` | Status bar bar |

### State Classes
| Class | Color | Effect |
|-------|-------|--------|
| `.status-green` | Green (#00ffcc / #27ae60) | Normal operation |
| `.status-yellow` | Yellow (#ffaa00 / #f2c94c) | Warning/caution |
| `.status-red` | Red (#ff3b3b / #eb5757) | Critical/error |

### Size Variants
| Size | Header | Value | Unit | Uses |
|------|--------|-------|------|------|
| `.size-sm` | 0.75rem | clamp(1.2-1.8rem) | 0.75rem | Compact dashboards |
| `.size-md` | 0.9rem | clamp(1.8-3rem) | 0.95rem | Default/standard |
| `.size-lg` | 1.1rem | clamp(2.5-3.5rem) | 1.1rem | Large displays |

---

## ✨ Design Features

### 1. Outer Glow Border
- **Type**: Radial gradient glow
- **Radius**: 20px blur spread
- **Opacity**: 0.15-0.4 depending on state
- **Color modes**: Green/yellow/red variants

### 2. Animated Status Footer
- **Height**: 4px (full-width bar)
- **Animation**: `glow-center-spread` 2s infinite
- **Gradient**: Center-spread (transparent → strong → transparent)
- **Effect**: Continuous pulse with blur animation

### 3. Floating Aesthetic
- **Hover lift**: translateY(-4px)
- **Transition**: all 0.2s ease-out
- **Glow enhance**: 30px blur on hover
- **Shadow elevate**: outer-lg instead of outer-md

### 4. Typography
- **Header**: Uppercase, 0.5px letter-spacing, dimmed
- **Value**: Monospace font (Courier New), bold, color-coded
- **Unit**: Secondary text color, smaller font
- **Shadows**: Text-shadow on value for glow effect

### 5. Responsive Scaling
- Values use `clamp(min, preferred, max)` for fluid sizing
- Relative to viewport width on mobile
- Maintains proportions across breakpoints

---

## 🎯 Use Cases

### Real-Time Monitoring
```html
<div class="monitor-card-value size-md status-green">
  <div class="monitor-card-value-header">Temperature</div>
  <div class="monitor-card-value-body">
    <div class="monitor-card-value-value">24.5</div>
    <div class="monitor-card-value-unit">°C</div>
  </div>
  <div class="monitor-card-value-footer"></div>
</div>
```

### System Metrics
```html
<div class="monitor-card-value size-md status-yellow">
  <div class="monitor-card-value-header">CPU Usage</div>
  <div class="monitor-card-value-body">
    <div class="monitor-card-value-value">75</div>
    <div class="monitor-card-value-unit">%</div>
  </div>
  <div class="monitor-card-value-footer"></div>
</div>
```

### Alert Display
```html
<div class="monitor-card-value size-md status-red">
  <div class="monitor-card-value-header">Errors</div>
  <div class="monitor-card-value-body">
    <div class="monitor-card-value-value">12</div>
    <div class="monitor-card-value-unit">alerts</div>
  </div>
  <div class="monitor-card-value-footer"></div>
</div>
```

---

## 🎨 CSS Customization

### Change Glow Color
```css
.monitor-card-value.status-blue {
  box-shadow:
    var(--shadow-outer-md),
    inset 0 0 0 1px rgba(100, 150, 255, 0.2),
    0 0 20px rgba(100, 150, 255, 0.2);
}

.monitor-card-value.status-blue .monitor-card-value-value {
  color: #6496ff;
}

.monitor-card-value.status-blue .monitor-card-value-footer::after {
  background: linear-gradient(90deg,
    transparent 0%,
    rgba(100, 150, 255, 0.6) 25%,
    rgba(100, 150, 255, 0.8) 50%,
    rgba(100, 150, 255, 0.6) 75%,
    transparent 100%
  );
}
```

### Adjust Animation Speed
```css
.monitor-card-value-footer::after {
  animation: glow-center-spread 1s ease-in-out infinite; /* Faster */
}
```

### Modify Border Effect
```css
.monitor-card-value {
  box-shadow:
    var(--shadow-outer-lg),  /* Stronger shadow */
    inset 0 0 0 2px rgba(0, 255, 204, 0.3),  /* Thicker border */
    0 0 30px rgba(0, 255, 204, 0.3);  /* Larger glow */
}
```

---

## 🔌 JavaScript Integration

### Update Value Dynamically
```javascript
// Update temperature value
document.querySelector('.monitor-card-value-value').textContent = '28.3';

// Change status color
const card = document.querySelector('.monitor-card-value');
card.classList.remove('status-green');
card.classList.add('status-yellow');
```

### Bind to WebSocket Data
```javascript
websocket.addEventListener('message', (event) => {
  const data = JSON.parse(event.data);
  
  // Update value
  document.getElementById('temp-display').textContent = data.temperature;
  
  // Update status based on value
  const card = event.target.closest('.monitor-card-value');
  if (data.temperature > 30) {
    card.className = 'monitor-card-value size-md status-red';
  } else if (data.temperature > 25) {
    card.className = 'monitor-card-value size-md status-yellow';
  } else {
    card.className = 'monitor-card-value size-md status-green';
  }
});
```

### React Component Example
```jsx
function MonitorCardValue({ label, value, unit, status = 'green' }) {
  return (
    <div className={`monitor-card-value size-md status-${status}`}>
      <div className="monitor-card-value-header">{label}</div>
      <div className="monitor-card-value-body">
        <div className="monitor-card-value-value">{value}</div>
        <div className="monitor-card-value-unit">{unit}</div>
      </div>
      <div className="monitor-card-value-footer"></div>
    </div>
  );
}
```

---

## 📊 Comparison with Related Components

| Component | Purpose | Border | Animation | Use Case |
|-----------|---------|--------|-----------|----------|
| **Monitor Card Value** | Premium display | Outer glow | Animated bar | KPI dashboards |
| Monitor Card Standard | Standard display | Left border | Status glow | Basic monitoring |
| Monitor Card Flat | Compact display | No border | Bottom glow | Dense dashboards |
| Chart Card | Visualization | Subtle glow | Floating line | Trend analysis |
| LED Indicator | Status only | Circular | Pulsing | Simple flags |

---

## 🌓 Theme Support

### Dark Mode (Default)
```css
.monitor-card-value {
  background-color: var(--color-bg-surface);  /* #1a1d26 */
  color: var(--color-text-primary);  /* #e8eef2 */
}

.monitor-card-value-value {
  color: var(--color-accent-green);  /* #00ffcc */
}
```

### Light Mode
```css
[data-theme="light"] .monitor-card-value {
  background-color: var(--color-bg-surface);  /* #ffffff */
  color: var(--color-text-primary);  /* #1a1a1a */
}

[data-theme="light"] .monitor-card-value-value {
  color: var(--color-accent-green);  /* #27ae60 */
}
```

---

## 📱 Responsive Behavior

### Mobile (320px)
```
┌─────────────────┐
│ Temperature     │  <- Uppercase label
│ 22.1            │  <- Scaled down value
│ °C              │
│ ═════════════   │  <- Status bar
└─────────────────┘
```
- Single column layout
- Size: 80% of container width
- Value font: 1.2-1.8rem (clamp)

### Tablet (768px)
```
┌─────────────────┬─────────────────┐
│ Temp  │ Humidity │ Pressure │ CPU │
│ 24.5  │    72    │   1013   │ 45  │
│ °C    │    %     │   hPa    │ %   │
└───────┴──────────┴──────────┴─────┘
```
- 2-column grid
- Size: size-md (default)

### Desktop (1024px+)
```
┌─────┬─────┬─────┬─────┬─────┬─────┐
│Temp │Humid│Press│CPU  │Mem  │Disk │
│24.5 │ 72  │1013 │ 45  │ 62  │ 78  │
│ °C  │ %   │hPa  │ %   │ %   │ %   │
└─────┴─────┴─────┴─────┴─────┴─────┘
```
- 3+ column grid (responsive)
- Size: size-md or size-lg
- Full hover effects enabled

---

## 🎯 Best Practices

### ✅ DO
- Use consistent units (°C, %, Mbps)
- Keep values to 1-3 decimal places
- Use appropriate status color (green/yellow/red)
- Display in responsive grid (auto-fit)
- Update values via JavaScript, not DOM manipulation

### ❌ DON'T
- Mix size variants in same row
- Use text longer than 20 characters for label
- Change status colors via inline styles
- Display values without units
- Overload with more than 2 lines of text

---

## 🔧 Troubleshooting

| Issue | Solution |
|-------|----------|
| Glow not visible | Check theme, ensure status class applied |
| Animation not smooth | Check browser hardware acceleration settings |
| Value text too small | Use size-lg variant or check viewport |
| Status bar not animating | Verify `glow-center-spread` keyframes in CSS |
| Colors wrong in light mode | Check `[data-theme="light"]` overrides |

---

## 📊 Component Statistics

| Metric | Value |
|--------|-------|
| **CSS Lines** | ~250 lines |
| **Keyframe Animations** | 1 (glow-center-spread) |
| **Size Variants** | 3 (sm, md, lg) |
| **Status States** | 3 (green, yellow, red) |
| **Box Shadows** | Multiple (outer, inner, glow) |
| **Responsive Breakpoints** | 3 (mobile, tablet, desktop) |
| **Dark/Light Support** | Yes (automatic via data-theme) |

---

## 🚀 Getting Started

### 1. Add to HTML
```html
<div class="monitor-card-value size-md status-green">
  <div class="monitor-card-value-header">Temperature</div>
  <div class="monitor-card-value-body">
    <div class="monitor-card-value-value">24.5</div>
    <div class="monitor-card-value-unit">°C</div>
  </div>
  <div class="monitor-card-value-footer"></div>
</div>
```

### 2. Update Values
```javascript
// Via JavaScript
document.querySelector('.monitor-card-value-value').textContent = newValue;

// Via data binding (Vue/React)
<MonitorCardValue value={temperature} unit="°C" status={temp > 30 ? 'red' : 'green'} />
```

### 3. Change Status
```javascript
element.className = 'monitor-card-value size-md status-yellow';
```

### 4. Style Customization
Edit CSS variables in `styles.css` or override in `components.css`

---

## 📚 Documentation References

- **Main Docs**: See [README.md](README.md)
- **v2.0 Features**: See [VERSION_2_SUMMARY.md](VERSION_2_SUMMARY.md)
- **Component Guide**: See [DASHBOARD_GUIDE.md](DASHBOARD_GUIDE.md)
- **Quick Reference**: See [QUICKSTART.md](QUICKSTART.md)

---

## 🎊 Summary

The **Monitor Card Value** component is a premium addition to the industrial UI library, bringing sophisticated glow borders and animated status indicators to real-time monitoring dashboards. With three size variants, three status colors, and full dark/light mode support, it's perfect for KPI displays, system metrics, and alert visualization.

**Features**:
- ✨ Outer glow border effect
- 🎬 Animated center-spread status bar
- 🌓 Dark/Light mode support
- 📱 Fully responsive
- 🎨 3 status colors + 3 sizes
- ⚡ Smooth 2s animations
- 🎯 Production ready

**Status**: ✅ **Complete & Ready for Deployment**

---

**Component 14/14** in the 3D Web UI Library  
**Revision**: v2.0+  
**Last Updated**: April 2, 2026
