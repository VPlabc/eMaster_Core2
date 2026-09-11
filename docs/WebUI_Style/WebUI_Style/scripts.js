/* ============================================================================
   3D Web UI Component System - JavaScript Interactions & State Management
   ========================================================================== */

'use strict';

/**
 * Component State Manager
 * Handles state for all interactive components
 */
class ComponentStateManager {
  constructor() {
    this.states = new Map();
    this.init();
  }

  init() {
    this.setupCheckboxes();
    this.setupInputs();
    this.setupButtons();
    this.setupTabs();
    this.setupGlowBackgrounds();
    this.setupLEDIndicators();
    this.setupRadioButtons();
    this.setupComboBoxes();
  }

  /**
   * Setup checkbox interactions
   */
  setupCheckboxes() {
    const checkboxes = document.querySelectorAll('.checkbox input[type="checkbox"]');
    checkboxes.forEach((checkbox) => {
      checkbox.addEventListener('change', (e) => {
        this.handleCheckboxChange(e);
      });

      checkbox.addEventListener('focus', (e) => {
        e.target.closest('.checkbox-input').style.boxShadow =
          'var(--shadow-outer-md), 0 0 10px rgba(0, 255, 204, 0.3)';
      });

      checkbox.addEventListener('blur', (e) => {
        e.target.closest('.checkbox-input').style.boxShadow = 'var(--shadow-inner-md)';
      });
    });
  }

  handleCheckboxChange(event) {
    const checkbox = event.target;
    const checkmark = checkbox.nextElementSibling.querySelector('.checkbox-checkmark');

    if (checkbox.checked) {
      console.log('Checkbox checked:', checkbox.id || checkbox.name);
      // Trigger animation
      checkmark.style.animation = 'none';
      setTimeout(() => {
        checkmark.style.animation = 'grow 150ms ease-out';
      }, 10);
    } else {
      console.log('Checkbox unchecked:', checkbox.id || checkbox.name);
    }
  }

  /**
   * Setup input field interactions
   */
  setupInputs() {
    const inputs = document.querySelectorAll('.input-field');

    inputs.forEach((input) => {
      input.addEventListener('focus', (e) => {
        this.updateInputState(e.target, 'focused');
      });

      input.addEventListener('blur', (e) => {
        this.updateInputState(e.target, 'blurred');
      });

      input.addEventListener('input', (e) => {
        this.updateInputState(e.target, 'filled');
      });
    });
  }

  updateInputState(input, state) {
    const group = input.closest('.input-group');

    switch (state) {
      case 'focused':
        input.style.boxShadow = 'var(--shadow-inner-md), var(--glow-soft)';
        break;
      case 'blurred':
        if (input.value.trim() === '') {
          input.style.boxShadow = 'var(--shadow-inner-md)';
        }
        break;
      case 'filled':
        if (input.value.trim() !== '') {
          input.style.boxShadow = 'var(--shadow-inner-md)';
          input.style.backgroundColor = 'var(--color-bg-highlight)';
        } else {
          input.style.backgroundColor = 'var(--color-bg-surface)';
        }
        break;
    }
  }

  /**
   * Setup button interactions
   */
  setupButtons() {
    const buttons = document.querySelectorAll('.btn');

    buttons.forEach((button) => {
      button.addEventListener('mousedown', (e) => {
        button.style.transform = 'translateY(0)';
        button.style.boxShadow = 'var(--shadow-inner-md)';
      });

      button.addEventListener('mouseup', (e) => {
        button.style.transform = 'translateY(-2px)';
        button.style.boxShadow = 'var(--shadow-outer-lg), var(--glow-soft)';
      });

      button.addEventListener('mouseleave', (e) => {
        button.style.transform = 'translateY(0)';
        button.style.boxShadow = 'var(--shadow-outer-md)';
      });

      button.addEventListener('click', (e) => {
        console.log('Button clicked:', button.textContent.trim());
      });
    });
  }

  /**
   * Setup tab switching
   */
  setupTabs() {
    const tabButtons = document.querySelectorAll('.tab-button');

    tabButtons.forEach((button) => {
      button.addEventListener('click', (e) => {
        e.preventDefault();
        this.switchTab(button);
      });
    });
  }

  switchTab(button) {
    const tabsContainer = button.closest('.tabs-container');
    const tabName = button.getAttribute('data-tab');

    // Remove active class from all buttons in this container
    tabsContainer.querySelectorAll('.tab-button').forEach((btn) => {
      btn.classList.remove('active');
    });

    // Add active class to clicked button
    button.classList.add('active');

    // Switch panel visibility
    const panelsContainer = tabsContainer.closest('.tabs-wrapper') || tabsContainer.parentElement;
    const allPanels = panelsContainer.querySelectorAll('.tab-panel');

    allPanels.forEach((panel) => {
      panel.classList.remove('active');
    });

    const activePanel = panelsContainer.querySelector(`.tab-panel[data-tab="${tabName}"]`);
    if (activePanel) {
      activePanel.classList.add('active');
      console.log('Tab switched to:', tabName);
    }
  }

  /**
   * Setup glow background interactions
   */
  setupGlowBackgrounds() {
    const glowBGs = document.querySelectorAll('.glow-background');

    glowBGs.forEach((bg) => {
      bg.addEventListener('mouseenter', () => {
        bg.classList.add('active');
      });

      bg.addEventListener('mouseleave', () => {
        bg.classList.remove('active');
      });

      bg.addEventListener('click', () => {
        bg.classList.toggle('active');
      });
    });
  }

  /**
   * Setup LED indicator interactions
   */
  setupLEDIndicators() {
    const ledIndicators = document.querySelectorAll('.led-indicator');

    ledIndicators.forEach((indicator) => {
      const ledLight = indicator.querySelector('.led-light');
      const states = ['state-green', 'state-yellow', 'state-red', 'state-off'];
      let currentStateIndex = 0;

      indicator.addEventListener('click', () => {
        // Remove all state classes
        states.forEach((state) => ledLight.classList.remove(state));

        // Cycle to next state
        currentStateIndex = (currentStateIndex + 1) % states.length;
        ledLight.classList.add(states[currentStateIndex]);

        console.log('LED state changed to:', states[currentStateIndex]);
      });
    });
  }
}

/**
 * Demo page utilities
 */
class DemoPageUtils {
  constructor() {
    this.init();
  }

  init() {
    this.setupCopyButtons();
    this.setupThemeToggle();
    this.setupSizeToggle();
    this.setupStateControls();
  }

  /**
   * Setup copy-to-clipboard functionality for code snippets
   */
  setupCopyButtons() {
    const copyButtons = document.querySelectorAll('.copy-button');

    copyButtons.forEach((button) => {
      button.addEventListener('click', (e) => {
        const codeBlock = button.closest('.code-snippet')?.querySelector('code');
        if (codeBlock) {
          const code = codeBlock.textContent;
          navigator.clipboard.writeText(code).then(() => {
            const originalText = button.textContent;
            button.textContent = '✓ Copied!';
            setTimeout(() => {
              button.textContent = originalText;
            }, 2000);
          });
        }
      });
    });
  }

  /**
   * Setup theme toggle (dark/light)
   */
  setupThemeToggle() {
    const themeToggle = document.querySelector('[data-theme-toggle]');
    if (!themeToggle) return;

    const savedTheme = localStorage.getItem('theme') || 'dark';
    this.setTheme(savedTheme);

    themeToggle.addEventListener('click', () => {
      const currentTheme = document.documentElement.getAttribute('data-theme');
      const newTheme = currentTheme === 'dark' ? 'light' : 'dark';
      this.setTheme(newTheme);
      localStorage.setItem('theme', newTheme);
    });
  }

  setTheme(theme) {
    document.documentElement.setAttribute('data-theme', theme);
    document.documentElement.style.colorScheme = theme;
    const themeToggle = document.querySelector('[data-theme-toggle]');
    if (themeToggle) {
      themeToggle.textContent = theme === 'dark' ? '☀️ Light Mode' : '🌙 Dark Mode';
      themeToggle.title = `Switch to ${theme === 'dark' ? 'Light' : 'Dark'} Mode`;
    }
  }

  /**
   * Setup size toggle for component variants
   */
  setupSizeToggle() {
    const sizeButtons = document.querySelectorAll('[data-size-toggle]');

    sizeButtons.forEach((button) => {
      button.addEventListener('click', (e) => {
        const size = button.getAttribute('data-size');
        const container = button.closest('.component-showcase');

        if (container) {
          container.querySelectorAll('[data-size]').forEach((comp) => {
            comp.className = comp.className
              .replace(/\bsize-(sm|md|lg)\b/g, '')
              .trim();
            comp.classList.add(`size-${size}`);
          });

          // Update active button
          button.parentElement?.querySelectorAll('[data-size-toggle]').forEach((btn) => {
            btn.classList.remove('active');
          });
          button.classList.add('active');
        }
      });
    });
  }

  /**
   * Setup state control buttons
   */
  setupStateControls() {
    const stateButtons = document.querySelectorAll('[data-state-toggle]');

    stateButtons.forEach((button) => {
      button.addEventListener('click', (e) => {
        const state = button.getAttribute('data-state');
        const target = button.getAttribute('data-target');
        const element = document.querySelector(target);

        if (element) {
          // Remove all state classes
          ['active', 'hover', 'disabled', 'error', 'state-green', 'state-yellow', 'state-red'].forEach((cls) => {
            element.classList.remove(cls);
          });

          // Add the selected state
          if (state !== 'default') {
            element.classList.add(state);
          }

          // Update active button
          button.parentElement?.querySelectorAll('[data-state-toggle]').forEach((btn) => {
            btn.classList.remove('active');
          });
          button.classList.add('active');
        }
      });
    });
  }
}

/**
 * Table interactions
 */
class TableInteractions {
  constructor() {
    this.init();
  }

  init() {
    const tables = document.querySelectorAll('table');
    tables.forEach((table) => {
      this.setupTableRowInteractions(table);
    });
  }

  setupTableRowInteractions(table) {
    const rows = table.querySelectorAll('tbody tr');

    rows.forEach((row) => {
      row.addEventListener('click', () => {
        rows.forEach((r) => r.style.backgroundColor = '');
        row.style.backgroundColor = 'var(--color-bg-highlight)';
        console.log('Row selected:', row.innerText.split('\t')[0]);
      });

      row.addEventListener('mouseenter', () => {
        row.style.boxShadow = 'inset 0 0 10px rgba(0, 255, 204, 0.1)';
      });

      row.addEventListener('mouseleave', () => {
        row.style.boxShadow = '';
      });
    });
  }
  /**
   * Setup radio button interactions
   */
  setupRadioButtons() {
    const radios = document.querySelectorAll('.radio-button input[type="radio"]');
    radios.forEach((radio) => {
      radio.addEventListener('change', (e) => {
        console.log('Radio button selected:', e.target.value);
      });
    });
  }

  /**
   * Setup combo box interactions
   */
  setupComboBoxes() {
    const comboBoxes = document.querySelectorAll('.combo-box-trigger');
    comboBoxes.forEach((trigger) => {
      const comboBox = trigger.closest('.combo-box');
      const dropdown = comboBox.querySelector('.combo-box-dropdown');
      const options = comboBox.querySelectorAll('.combo-box-option');

      trigger.addEventListener('click', () => {
        comboBox.classList.toggle('open');
      });

      options.forEach((option) => {
        option.addEventListener('click', () => {
          trigger.textContent = option.textContent;
          options.forEach((opt) => opt.classList.remove('selected'));
          option.classList.add('selected');
          comboBox.classList.remove('open');
          console.log('Combo box selection:', option.textContent);
        });
      });
    });

    // Close dropdown when clicking outside
    document.addEventListener('click', (e) => {
      comboBoxes.forEach((trigger) => {
        const comboBox = trigger.closest('.combo-box');
        if (!comboBox.contains(e.target)) {
          comboBox.classList.remove('open');
        }
      });
    });
  }}

/**
 * Animation performance optimization
 */
class AnimationOptimizer {
  constructor() {
    this.prefersReducedMotion = window.matchMedia('(prefers-reduced-motion: reduce)').matches;
    this.init();
  }

  init() {
    if (this.prefersReducedMotion) {
      document.documentElement.style.setProperty('--transition-fast', '0.01ms');
      document.documentElement.style.setProperty('--transition-base', '0.01ms');
      document.documentElement.style.setProperty('--transition-slow', '0.01ms');
      console.log('Reduced motion detected - animations disabled');
    }

    // Monitor for changes
    window.matchMedia('(prefers-reduced-motion: reduce)').addEventListener('change', (e) => {
      if (e.matches) {
        this.prefersReducedMotion = true;
        console.log('Reduced motion enabled');
      }
    });
  }
}

/**
 * Component showcase generator
 */
class ComponentShowcaseGenerator {
  constructor() {
    this.generateShowcaseCode();
    this.setupLiveCodeBlocks();
  }

  generateShowcaseCode() {
    const codeBlocks = document.querySelectorAll('[data-component-code]');

    codeBlocks.forEach((block) => {
      const component = block.getAttribute('data-component-code');
      const code = this.getComponentCode(component);

      if (block.querySelector('code')) {
        block.querySelector('code').textContent = code;
      }
    });
  }

  getComponentCode(component) {
    const codeSnippets = {
      checkbox: `<label class="checkbox">
  <input type="checkbox" id="demo-checkbox">
  <div class="checkbox-input">
    <div class="checkbox-checkmark"></div>
  </div>
  <span class="checkbox-label">Accept terms</span>
</label>`,

      input: `<div class="input-group">
  <label class="input-label">Email Address</label>
  <input type="email" class="input-field" placeholder="Enter your email">
  <div class="input-helper">We'll never share your email</div>
</div>`,

      button: `<button class="btn btn-primary size-md">
  Click Me
</button>`,

      table: `<div class="table-wrapper">
  <table>
    <thead>
      <tr>
        <th>Name</th>
        <th>Status</th>
        <th>Value</th>
      </tr>
    </thead>
    <tbody>
      <tr>
        <td>Item 1</td>
        <td>Active</td>
        <td>100</td>
      </tr>
    </tbody>
  </table>
</div>`,

      'card-monitor': `<div class="card-monitor size-md status-green">
  <div class="card-monitor-header">System Status</div>
  <div class="card-monitor-body">
    All systems operational
  </div>
  <div class="card-monitor-status-bar">
    <span class="status-indicator green"></span>
    <span>Online</span>
  </div>
</div>`,

      led: `<div class="led-indicator">
  <div class="led-light state-green"></div>
  <span class="led-label">Active</span>
</div>`,

      tabs: `<div class="tabs-container">
  <button class="tab-button active" data-tab="tab1">Tab 1</button>
  <button class="tab-button" data-tab="tab2">Tab 2</button>
</div>
<div class="tab-panel active" data-tab="tab1">
  Content 1
</div>
<div class="tab-panel" data-tab="tab2">
  Content 2
</div>`,

      glow: `<div class="glow-background size-md glow-green">
  <div class="glow-background-content">
    Glow Effect
  </div>
</div>`,
    };

    return codeSnippets[component] || 'Component code not found';
  }

  setupLiveCodeBlocks() {
    const liveBlocks = document.querySelectorAll('[data-live-code]');

    liveBlocks.forEach((block) => {
      const component = block.getAttribute('data-live-code');
      const html = this.getComponentCode(component);

      // Parse and render the HTML
      const container = block.querySelector('.code-preview');
      if (container) {
        container.innerHTML = html;

        // Re-initialize component interactions for newly created elements
        setTimeout(() => {
          const stateManager = new ComponentStateManager();
          const tableInteractions = new TableInteractions();
        }, 100);
      }
    });
  }
}

/**
 * Initialize everything when DOM is ready
 */
document.addEventListener('DOMContentLoaded', () => {
  console.log('🎨 3D Web UI Component System initialized');

  // Initialize all systems
  const stateManager = new ComponentStateManager();
  const demoUtils = new DemoPageUtils();
  const tableInteractions = new TableInteractions();
  const animationOptimizer = new AnimationOptimizer();
  const showcaseGenerator = new ComponentShowcaseGenerator();

  console.log('✅ All components ready');

  // Log component info
  console.log(`
    📊 Components loaded:
    ✓ Checkboxes
    ✓ Input Fields
    ✓ Buttons
    ✓ Tables
    ✓ Card Monitors
    ✓ LED Indicators
    ✓ Tabs
    ✓ Glow Backgrounds
  `);
});

/**
 * Utility functions for external use
 */
window.ComponentSystem = {
  /**
   * Create a new checkbox programmatically
   */
  createCheckbox(options = {}) {
    const { id = '', label = 'Checkbox', checked = false } = options;
    const checkbox = document.createElement('label');
    checkbox.className = 'checkbox';
    checkbox.innerHTML = `
      <input type="checkbox" id="${id}" ${checked ? 'checked' : ''}>
      <div class="checkbox-input">
        <div class="checkbox-checkmark"></div>
      </div>
      <span class="checkbox-label">${label}</span>
    `;
    return checkbox;
  },

  /**
   * Create a new button programmatically
   */
  createButton(options = {}) {
    const { text = 'Button', variant = '', size = 'md', onClick = () => {} } = options;
    const btn = document.createElement('button');
    btn.className = `btn ${variant} size-${size}`;
    btn.textContent = text;
    btn.addEventListener('click', onClick);
    return btn;
  },

  /**
   * Create an input field programmatically
   */
  createInput(options = {}) {
    const { label = '', placeholder = '', type = 'text', size = 'md' } = options;
    const group = document.createElement('div');
    group.className = 'input-group';
    group.innerHTML = `
      ${label ? `<label class="input-label">${label}</label>` : ''}
      <input type="${type}" class="input-field size-${size}" placeholder="${placeholder}">
    `;
    return group;
  },

  /**
   * Toggle component state
   */
  toggleComponent(elementId, state) {
    const element = document.getElementById(elementId);
    if (element) {
      element.classList.toggle(state);
    }
  },

  /**
   * Log all active states
   */
  logStates() {
    console.log('Active component states:');
    document.querySelectorAll('.checkbox input:checked').forEach((cb) => {
      console.log('  ☑️  Checkbox:', cb.id || cb.name);
    });
    document.querySelectorAll('.btn.active').forEach((btn) => {
      console.log('  🔘 Button:', btn.textContent.trim());
    });
    document.querySelectorAll('.tab-button.active').forEach((tab) => {
      console.log('  📑 Tab:', tab.textContent.trim());
    });
  },
};

/* ============================================================================
   CARD-BASED NOTIFICATION SYSTEM (Replaces modals & toasts)
   ========================================================================== */

// Ensure notification container exists
function ensureNotificationContainer() {
  let container = document.querySelector('.notification-container');
  if (!container) {
    container = document.createElement('div');
    container.classList.add('notification-container');
    document.body.appendChild(container);
  }
  return container;
}

// Show card-based notification
function showNotification(type, title, message, duration = 5000) {
  const container = ensureNotificationContainer();
  const card = document.createElement('div');
  card.classList.add('notification-card', type);
  
  const icons = { 'success': '✓', 'error': '✕', 'warning': '⚠', 'info': 'ℹ' };
  const html = `
    <div class="notification-header">
      <div class="notification-icon">${icons[type] || '•'}</div>
      <h3>${title}</h3>
      <button class="notification-close">&times;</button>
    </div>
    <div class="notification-body">${message}</div>
    <div class="notification-footer"></div>
  `;
  
  card.innerHTML = html;
  container.appendChild(card);
  setTimeout(() => card.classList.add('show'), 10);
  
  card.querySelector('.notification-close').addEventListener('click', () => {
    card.classList.remove('show');
    setTimeout(() => card.remove(), 300);
  });
  
  if (duration > 0) {
    setTimeout(() => {
      card.classList.remove('show');
      setTimeout(() => card.remove(), 300);
    }, duration);
  }
  return card;
}

// Legacy compatibility functions
function showModal(type = 'success') {
  const configs = {
    success: { icon: '🎉', title: 'Success', message: 'Operation completed successfully!', type: 'success' },
    error: { icon: '❌', title: 'Error', message: 'An error occurred while processing your request.', type: 'error' },
    warning: { icon: '⚠️', title: 'Warning', message: 'Please review your action before proceeding.', type: 'warning' },
    loading: { icon: '⏳', title: 'Processing', message: 'Please wait while we process your request...', type: 'info' }
  };
  const cfg = configs[type] || configs.success;
  
  // Update modal content
  const modalIcon = document.getElementById('modal-icon');
  const modalTitle = document.getElementById('modal-title');
  const modalBody = document.getElementById('modal-body');
  const modalOverlay = document.getElementById('modal-overlay');
  
  if (modalIcon) modalIcon.textContent = cfg.icon;
  if (modalTitle) modalTitle.textContent = cfg.title;
  if (modalBody) modalBody.textContent = cfg.message;
  
  // Show modal overlay
  if (modalOverlay) {
    modalOverlay.style.display = 'flex';
  }
}

function closeModal() {
  const modalOverlay = document.getElementById('modal-overlay');
  if (modalOverlay) {
    modalOverlay.style.display = 'none';
  }
}

function showToast(type = 'success', title = '', message = '') {
  showNotification(type, title || 'Notification', message || 'No message provided', 5000);
}

function closeToast() {
  const toast = document.getElementById('toast');
  if (toast) {
    toast.classList.remove('show');
  }
}

/* ============================================================================
   DEMO DATA & MAPPING FUNCTIONS
   ========================================================================== */

// Demo Data Structure
const MappingDemo = {
  sources: [
    { id: 'plc_1', type: 'PLC', name: 'PLC_1', address: '192.168.1.10', status: 'connected', variables: [
      { key: 'TEMP_1', address: '40001', type_data: 'float', value: 26.5, unit: '°C' },
      { key: 'HUM_1', address: '40002', type_data: 'float', value: 65.2, unit: '%' }
    ]},
    { id: 'opcua_1', type: 'OPC_UA', name: 'OPC_UA_Server', address: 'opc.tcp://localhost:4840', status: 'connected', variables: [
      { key: 'PRESSURE', address: 'ns=2;i=1001', type_data: 'float', value: 1.02, unit: 'bar' }
    ]},
    { id: 'mqtt_1', type: 'MQTT', name: 'MQTT_Broker', address: 'mqtt.example.com', status: 'connected', variables: [
      { key: 'DEVICE_STATE', address: 'home/device/state', type_data: 'string', value: 'ACTIVE', unit: '' }
    ]},
    { id: 'http_1', type: 'HTTP', name: 'REST_API', address: 'https://api.example.com', status: 'connected', variables: [
      { key: 'SENSOR_READING', address: '/sensor/temp', type_data: 'float', value: 24.8, unit: '°C' }
    ]}
  ],
  
  internal_vars: [
    { name: 'room_temp', type: 'float', display_unit: '°C', default_value: 0 },
    { name: 'room_humidity', type: 'float', display_unit: '%', default_value: 0 },
    { name: 'system_pressure', type: 'float', display_unit: 'bar', default_value: 0 },
    { name: 'device_state', type: 'string', display_unit: '', default_value: 'UNKNOWN' },
    { name: 'status_indicator', type: 'boolean', display_unit: '', default_value: false }
  ],
  
  mapping_rules: [
    { id: 1, value_name: 'room_temp', data_source: 'PLC_1.TEMP_1', internal_var: 'room_temp', ui_type: 'display', status: 'active' },
    { id: 2, value_name: 'system_pressure', data_source: 'OPC_UA_Server.PRESSURE', internal_var: 'system_pressure', ui_type: 'display', status: 'active' }
  ]
};

// Initialize Data
function initData() {
  console.log('Initializing demo data...', MappingDemo);
  
  // Populate source selector
  const sourceSelect = document.getElementById('data-source');
  if (sourceSelect) {
    sourceSelect.innerHTML = '<option value="">-- Select Data Source --</option>';
    MappingDemo.sources.forEach(source => {
      source.variables.forEach(variable => {
        const option = document.createElement('option');
        option.value = `${source.name}.${variable.key}`;
        option.textContent = `${source.type} - ${source.name}.${variable.key} (${variable.value}${variable.unit})`;
        sourceSelect.appendChild(option);
      });
    });
  }
  
  // Populate internal variable selector
  const internalVarSelect = document.getElementById('internal-var');
  if (internalVarSelect) {
    internalVarSelect.innerHTML = '<option value="">-- Select Internal Variable --</option>';
    MappingDemo.internal_vars.forEach(variable => {
      const option = document.createElement('option');
      option.value = variable.name;
      option.textContent = `${variable.name} (${variable.type})`;
      internalVarSelect.appendChild(option);
    });
  }
  
  // Render existing mappings
  renderMappingTable();
  generateMonitorUI();
  
  showNotification('success', 'Data Initialized', '✓ Demo data loaded successfully', 3000);
}

// Add Mapping
function addMapping(value_name, data_source, internal_var, ui_type = 'display') {
  if (!value_name || !data_source || !internal_var) {
    showNotification('warning', 'Invalid Input', '⚠ Please fill all fields', 4000);
    return false;
  }
  
  // Validate uniqueness
  if (MappingDemo.mapping_rules.some(r => r.internal_var === internal_var)) {
    showNotification('error', 'Duplicate Mapping', '✕ This variable is already mapped', 4000);
    return false;
  }
  
  const newMapping = {
    id: Math.max(...MappingDemo.mapping_rules.map(r => r.id), 0) + 1,
    value_name: value_name,
    data_source: data_source,
    internal_var: internal_var,
    ui_type: ui_type,
    status: 'active'
  };
  
  MappingDemo.mapping_rules.push(newMapping);
  
  // Clear form
  document.getElementById('mapping-value-name').value = '';
  document.getElementById('data-source').value = '';
  document.getElementById('internal-var').value = '';
  document.getElementById('ui-type').value = 'display';
  
  renderMappingTable();
  generateMonitorUI();
  
  showNotification('success', 'Mapping Added', `✓ New mapping created: ${internal_var}`, 3000);
  return true;
}

// Remove Mapping
function removeMapping(id) {
  const index = MappingDemo.mapping_rules.findIndex(r => r.id === id);
  if (index !== -1) {
    const removed = MappingDemo.mapping_rules.splice(index, 1)[0];
    renderMappingTable();
    generateMonitorUI();
    showNotification('info', 'Mapping Removed', `ℹ Removed: ${removed.internal_var}`, 3000);
    return true;
  }
  return false;
}

// Render Mapping Table
function renderMappingTable() {
  const tableBody = document.getElementById('mapping-table-body');
  if (!tableBody) return;
  
  tableBody.innerHTML = '';
  
  if (MappingDemo.mapping_rules.length === 0) {
    tableBody.innerHTML = '<tr><td colspan="6" style="text-align: center; color: var(--color-text-muted); padding: 2rem;">No mappings yet. Add one to get started.</td></tr>';
    return;
  }
  
  MappingDemo.mapping_rules.forEach(mapping => {
    const row = document.createElement('tr');
    row.innerHTML = `
      <td>${mapping.id}</td>
      <td>${mapping.value_name}</td>
      <td>${mapping.data_source}</td>
      <td>${mapping.internal_var}</td>
      <td>
        <span class="status-badge" style="background: rgba(0,255,150,0.15); color: var(--color-accent-green); padding: 0.3rem 0.8rem; border-radius: 4px; font-size: 0.85rem;">
          ${mapping.status.toUpperCase()}
        </span>
      </td>
      <td style="text-align: center;">
        <button onclick="removeMapping(${mapping.id})" style="background: none; border: none; color: var(--color-accent-red); cursor: pointer; padding: 0.2rem 0.6rem; font-weight: bold; font-size: 0.9rem;">
          ✕
        </button>
      </td>
    `;
    tableBody.appendChild(row);
  });
}

// Generate Monitor UI
function generateMonitorUI() {
  const monitorContainer = document.getElementById('generated-monitor-ui');
  if (!monitorContainer) return;
  
  monitorContainer.innerHTML = '';
  
  if (MappingDemo.mapping_rules.length === 0) {
    monitorContainer.innerHTML = '<p style="color: var(--color-text-muted); text-align: center; padding: 2rem;">Add mappings to see generated UI</p>';
    return;
  }
  
  MappingDemo.mapping_rules.forEach(mapping => {
    // Build a monitor-style card (revert to previous monitor layout)
    const containerEl = document.createElement('div');

    // Extract source value
    const sourceFullName = mapping.data_source;
    let sourceValue = 'N/A';
    let sourceUnit = '';

    MappingDemo.sources.forEach(source => {
      source.variables.forEach(variable => {
        if (`${source.name}.${variable.key}` === sourceFullName) {
          sourceValue = variable.value;
          sourceUnit = variable.unit || '';
        }
      });
    });

    // Determine status class
    const statusClass = (mapping.status === 'active') ? 'status-green' : (mapping.status === 'warning' ? 'status-yellow' : (mapping.status === 'error' ? 'status-red' : ''));

    containerEl.className = `monitor-card-standard size-md ${statusClass}`.trim();
    containerEl.innerHTML = `
      <div class="monitor-card-standard-header">${mapping.internal_var}</div>
      <div class="monitor-card-standard-body">
        <div class="monitor-card-standard-value">${sourceValue}</div>
        <div class="monitor-card-standard-unit">${sourceUnit}</div>
      </div>
      <div class="monitor-card-standard-footer" style="display:flex;align-items:center;justify-content:space-between;padding:0.6rem 1rem">
        <div style="display:flex;align-items:center;gap:0.5rem"><span class="status-indicator ${mapping.status === 'active' ? 'green' : mapping.status === 'warning' ? 'yellow' : mapping.status === 'error' ? 'red' : ''}"></span><small style="color:var(--color-text-secondary);font-weight:600">${mapping.data_source}</small></div>
        <div><small style="color:var(--color-text-muted)">UI: ${mapping.ui_type}</small></div>
      </div>
    `;

    monitorContainer.appendChild(containerEl);
  });
}

// Wire up buttons if on mapping page
function setupMappingPage() {
  const addButton = document.getElementById('add-mapping-btn');
  if (addButton) {
    addButton.addEventListener('click', () => {
      const value_name = document.getElementById('mapping-value-name').value;
      const data_source = document.getElementById('data-source').value;
      const internal_var = document.getElementById('internal-var').value;
      const ui_type = document.getElementById('ui-type').value;
      
      addMapping(value_name, data_source, internal_var, ui_type);
    });
  }
  
  // Initialize on page load
  initData();
}

// Initialize when document is ready
if (document.readyState === 'loading') {
  document.addEventListener('DOMContentLoaded', () => {
    if (window.location.pathname.includes('mapping')) {
      setupMappingPage();
    }
  });
} else {
  if (window.location.pathname.includes('mapping')) {
    setupMappingPage();
  }
}

/* ============================================================================
   ENHANCED COMPONENTS (v2.1+)
   Advanced functionality for chart cards, modals, and combo boxes
   ========================================================================== */

/**
 * Enhanced Combo Box - Interactive dropdown with smooth animations
 */
window.EnhancedComboBox = {
  init() {
    const comboBoxes = document.querySelectorAll('.combo-box-enhanced');
    comboBoxes.forEach(comboBox => {
      const trigger = comboBox.querySelector('.combo-box-enhanced-trigger');
      const dropdown = comboBox.querySelector('.combo-box-enhanced-dropdown');
      const options = comboBox.querySelectorAll('.combo-box-enhanced-option');

      if (!trigger) return;

      trigger.addEventListener('click', () => {
        this.toggleDropdown(comboBox);
      });

      options.forEach(option => {
        option.addEventListener('click', () => {
          this.selectOption(comboBox, option);
        });
      });

      document.addEventListener('click', (e) => {
        if (!comboBox.contains(e.target) && comboBox.classList.contains('open')) {
          this.closeDropdown(comboBox);
        }
      });
    });
  },

  toggleDropdown(comboBox) {
    if (comboBox.classList.contains('open')) {
      this.closeDropdown(comboBox);
    } else {
      this.openDropdown(comboBox);
    }
  },

  openDropdown(comboBox) {
    comboBox.classList.add('open');
  },

  closeDropdown(comboBox) {
    comboBox.classList.remove('open');
  },

  selectOption(comboBox, option) {
    const trigger = comboBox.querySelector('.combo-box-enhanced-trigger');
    const optionText = option.textContent;

    trigger.firstElementChild.textContent = optionText;
    comboBox.querySelectorAll('.combo-box-enhanced-option').forEach(opt => {
      opt.classList.remove('selected');
    });
    option.classList.add('selected');
    this.closeDropdown(comboBox);

    console.log('Selected option:', optionText);
  }
};

/**
 * Enhanced Modal System
 */
window.EnhancedModal = {
  createProgressCircle(percentage = 45) {
    const circle = document.createElement('div');
    circle.className = 'progress-circle';
    circle.style.setProperty('--progress', percentage / 100);
    circle.textContent = `${percentage}%`;
    return circle;
  },

  createSpinner() {
    const spinner = document.createElement('div');
    spinner.className = 'modal-spinner';
    return spinner;
  },

  showSettingValueModal(title, currentValue, onConfirm) {
    const modal = document.createElement('div');
    modal.className = 'modal-enhanced modal';
    modal.innerHTML = `
      <div class="modal-header">
        <h2>${title}</h2>
        <button class="modal-close">×</button>
      </div>
      <div class="modal-body">
        <div class="setting-value-modal">
          <div class="setting-value-input">
            <label class="setting-value-label">Current Value</label>
            <input type="text" class="setting-value-field" id="setting-input" value="${currentValue}" />
          </div>
        </div>
      </div>
      <div class="modal-footer">
        <button class="modal-btn">Cancel</button>
        <button class="modal-btn primary" id="confirm-btn">Confirm</button>
      </div>
    `;

    const overlay = document.createElement('div');
    overlay.className = 'modal-overlay';
    overlay.appendChild(modal);
    // Ensure overlay is visible (components.css hides modal-overlay by default)
    overlay.style.display = 'flex';
    document.body.appendChild(overlay);

    const input = modal.querySelector('#setting-input');
    const confirmBtn = modal.querySelector('#confirm-btn');
    const closeBtn = modal.querySelector('.modal-close');
    const cancelBtn = modal.querySelectorAll('.modal-btn')[0];

    input.focus();
    input.select();

    const cleanup = () => overlay.remove();

    confirmBtn.addEventListener('click', () => {
      if (onConfirm) onConfirm(input.value);
      cleanup();
    });

    closeBtn.addEventListener('click', cleanup);
    cancelBtn.addEventListener('click', cleanup);
    overlay.addEventListener('click', (e) => {
      if (e.target === overlay) cleanup();
    });

    input.addEventListener('keypress', (e) => {
      if (e.key === 'Enter') confirmBtn.click();
    });
  },

  showConfigAddItemModal(onAdd) {
    const modal = document.createElement('div');
    modal.className = 'modal-enhanced modal';
    modal.style.minWidth = '500px';
    modal.innerHTML = `
      <div class="modal-header">
        <h2>Add Configuration Item</h2>
        <button class="modal-close">×</button>
      </div>
      <div class="modal-body">
        <div class="config-add-item" id="config-fields">
          <div class="config-field-group">
            <label class="config-field-label">Field Name</label>
            <input type="text" class="config-field-input" placeholder="e.g., Protocol" />
          </div>
          <div class="config-field-group">
            <label class="config-field-label">Field Value</label>
            <input type="text" class="config-field-input" placeholder="e.g., MQTT" />
          </div>
        </div>
        <button class="config-add-field-btn" id="add-more-btn">+ Add More Fields</button>
      </div>
      <div class="modal-footer">
        <button class="modal-btn">Cancel</button>
        <button class="modal-btn primary" id="save-btn">Save Configuration</button>
      </div>
    `;

    const overlay = document.createElement('div');
    overlay.className = 'modal-overlay';
    overlay.appendChild(modal);
    // Make overlay visible immediately
    overlay.style.display = 'flex';
    document.body.appendChild(overlay);

    const closeBtn = modal.querySelector('.modal-close');
    const cancelBtn = modal.querySelectorAll('.modal-btn')[0];
    const saveBtn = modal.querySelector('#save-btn');
    const addMoreBtn = modal.querySelector('#add-more-btn');
    const configFields = modal.querySelector('#config-fields');

    const cleanup = () => overlay.remove();

    closeBtn.addEventListener('click', cleanup);
    cancelBtn.addEventListener('click', cleanup);
    overlay.addEventListener('click', (e) => {
      if (e.target === overlay) cleanup();
    });

    addMoreBtn.addEventListener('click', () => {
      const newField = document.createElement('div');
      newField.className = 'config-field-group';
      newField.innerHTML = `
        <label class="config-field-label">Field Name</label>
        <input type="text" class="config-field-input" placeholder="Field name" />
        <label class="config-field-label">Field Value</label>
        <input type="text" class="config-field-input" placeholder="Field value" />
      `;
      configFields.insertBefore(newField, addMoreBtn);
    });

    saveBtn.addEventListener('click', () => {
      const fields = [];
      const fieldGroups = modal.querySelectorAll('.config-field-group');
      fieldGroups.forEach(group => {
        const inputs = group.querySelectorAll('.config-field-input');
        if (inputs.length >= 2) {
          fields.push({
            name: inputs[0].value,
            value: inputs[1].value
          });
        }
      });
      if (onAdd) onAdd(fields);
      cleanup();
    });
  }
};

/**
 * Chart Card Animation Enhancement
 */
window.ChartCardEnhanced = {
  createAnimatedChart(container, data, options = {}) {
    // Supports:
    // - data: array of numbers => single-series
    // - data: array of { name, data: [...], color } => multi-series
    const cfg = Object.assign({ width: 340, height: 160, padding: 12, smooth: true, startTime: new Date(), timeStepMinutes: 60 }, options);

    // Normalize series
    let series = [];
    if (!data) data = [];
    if (Array.isArray(data) && data.length > 0 && typeof data[0] === 'number') {
      series = [{ name: 'Series 1', data: data.slice(), color: 'rgba(0,255,204,0.9)' }];
    } else if (Array.isArray(data)) {
      series = data.map((s, idx) => ({ name: s.name || `Series ${idx+1}`, data: s.data || [], color: s.color || getDefaultColor(idx) }));
    }

    const w = cfg.width, h = cfg.height, p = cfg.padding;
    const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
    svg.setAttribute('viewBox', `0 0 ${w} ${h}`);
    svg.setAttribute('width', String(w));
    svg.setAttribute('height', String(h));
    svg.setAttribute('class', 'chart-svg-enhanced');

    // Shared defs
    const defs = document.createElementNS('http://www.w3.org/2000/svg', 'defs');
    svg.appendChild(defs);

    // Calculate scales
    const allValues = series.flatMap(s => s.data);
    const maxV = Math.max(1, ...allValues);
    const minV = Math.min(0, ...allValues);

    // Container for lines and points
    const gLines = document.createElementNS('http://www.w3.org/2000/svg', 'g');
    svg.appendChild(gLines);

    // Tooltip element (HTML overlay)
    const tooltip = document.createElement('div');
    tooltip.className = 'chart-tooltip';
    tooltip.style.position = 'absolute';
    tooltip.style.pointerEvents = 'none';
    tooltip.style.opacity = '0';
    tooltip.style.transform = 'translate(-50%, -120%) scale(0.95)';
    tooltip.style.transition = 'all 0.18s ease-out';
    tooltip.style.zIndex = '1000';
    tooltip.style.padding = '8px 10px';
    tooltip.style.borderRadius = '8px';
    tooltip.style.background = 'rgba(15,17,23,0.95)';
    tooltip.style.border = '1px solid rgba(0,255,204,0.14)';
    tooltip.style.color = 'var(--color-text-primary)';
    tooltip.style.fontSize = '12px';
    tooltip.style.fontWeight = '600';
    tooltip.innerHTML = '<div class="tt-value" style="color:var(--color-accent-green);"></div><div class="tt-meta" style="color:var(--color-text-secondary); font-size:11px; font-weight:500; margin-top:4px"></div>';

    // Helper to map value to coordinates
    function xFor(i, len) { return p + (i / Math.max(1, len - 1)) * (w - p * 2); }
    function yFor(v) { return h - p - ((v - minV) / Math.max(1, maxV - minV)) * (h - p * 2); }

    // Smooth path helper: Catmull-Rom to Bezier
    function catmullRom2bezier(points) {
      const d = [];
      for (let i = 0; i < points.length; i++) {
        const p0 = points[i - 1] || points[i];
        const p1 = points[i];
        const p2 = points[i + 1] || p1;
        const p3 = points[i + 2] || p2;
        if (i === 0) d.push(`M ${p1.x} ${p1.y}`);
        const cp1x = p1.x + (p2.x - p0.x) / 6;
        const cp1y = p1.y + (p2.y - p0.y) / 6;
        const cp2x = p2.x - (p3.x - p1.x) / 6;
        const cp2y = p2.y - (p3.y - p1.y) / 6;
        d.push(`C ${cp1x} ${cp1y} ${cp2x} ${cp2y} ${p2.x} ${p2.y}`);
      }
      return d.join(' ');
    }

    // Render each series
    series.forEach((s, si) => {
      const vals = s.data;
      if (!vals || vals.length === 0) return;
      const points = vals.map((v, i) => ({ x: xFor(i, vals.length), y: yFor(v), v, i }));

      // Path
      const path = document.createElementNS('http://www.w3.org/2000/svg', 'path');
      const d = cfg.smooth ? catmullRom2bezier(points) : ('M ' + points.map(p => `${p.x} ${p.y}`).join(' L '));
      path.setAttribute('d', d);
      path.setAttribute('fill', 'none');
      path.setAttribute('stroke', s.color || getDefaultColor(si));
      path.setAttribute('stroke-width', '3');
      path.setAttribute('class', 'chart-line-enhanced');
      path.style.strokeLinecap = 'round';
      path.style.strokeLinejoin = 'round';
      gLines.appendChild(path);

      // animate stroke draw
      requestAnimationFrame(() => {
        const len = path.getTotalLength();
        path.style.transition = 'none';
        path.style.strokeDasharray = len;
        path.style.strokeDashoffset = len;
        requestAnimationFrame(() => {
          path.style.transition = 'stroke-dashoffset 900ms ease-out';
          path.style.strokeDashoffset = '0';
        });
      });

      // Points
      points.forEach(pt => {
        const circle = document.createElementNS('http://www.w3.org/2000/svg', 'circle');
        circle.setAttribute('cx', pt.x);
        circle.setAttribute('cy', pt.y);
        circle.setAttribute('r', '4');
        circle.setAttribute('fill', s.color || getDefaultColor(si));
        circle.setAttribute('stroke', 'rgba(0,0,0,0.2)');
        circle.setAttribute('data-series', si);
        circle.setAttribute('data-index', pt.i);
        circle.setAttribute('data-value', pt.v);
        circle.style.cursor = 'pointer';
        circle.classList.add('chart-point');

        // events
        circle.addEventListener('mouseenter', (ev) => {
          const val = Number(circle.getAttribute('data-value'));
          const idx = Number(circle.getAttribute('data-index'));
          const seriesName = s.name;
          const ttValue = tooltip.querySelector('.tt-value');
          const ttMeta = tooltip.querySelector('.tt-meta');
          ttValue.textContent = `${seriesName}: ${formatNumber(val)}`;

          // date/time calculation
          const t = new Date(cfg.startTime.getTime() + idx * cfg.timeStepMinutes * 60000);
          const dateStr = t.toLocaleDateString();
          const timeStr = t.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
          ttMeta.innerHTML = `${dateStr} • ${timeStr}`;
          tooltip.style.opacity = '1';
          tooltip.style.transform = 'translate(-50%, -120%) scale(1)';
        });

        circle.addEventListener('mousemove', (ev) => {
          const containerRect = container.getBoundingClientRect();
          tooltip.style.left = (ev.clientX - containerRect.left) + 'px';
          tooltip.style.top = (ev.clientY - containerRect.top - 12) + 'px';
        });

        circle.addEventListener('mouseleave', () => {
          tooltip.style.opacity = '0';
          tooltip.style.transform = 'translate(-50%, -120%) scale(0.95)';
        });

        gLines.appendChild(circle);
      });
    });

    // Legend (if multiple series)
    if (series.length > 1) {
      const legend = document.createElement('div');
      legend.className = 'chart-legend';
      legend.style.display = 'flex';
      legend.style.gap = '8px';
      legend.style.position = 'absolute';
      legend.style.right = '8px';
      legend.style.top = '8px';
      legend.style.zIndex = '10';
      series.forEach((s, si) => {
        const item = document.createElement('div');
        item.style.display = 'flex';
        item.style.alignItems = 'center';
        item.style.gap = '6px';
        item.innerHTML = `<span style="width:10px;height:10px;border-radius:3px;background:${s.color}"></span><span style="font-size:11px;color:var(--color-text-secondary)">${s.name}</span>`;
        legend.appendChild(item);
      });
      // legend will be attached to container
      container.style.position = 'relative';
      container.appendChild(legend);
    }

    // Attach tooltip and svg
    container.style.position = 'relative';
    container.appendChild(svg);
    container.appendChild(tooltip);

    return { svg, tooltip };
  }
};

// Global helper to attach a progress circle into any container
window.showProgressCircle = function(containerSelectorOrEl, percentage = 45) {
  const container = typeof containerSelectorOrEl === 'string' ? document.querySelector(containerSelectorOrEl) : containerSelectorOrEl;
  if (!container) return null;
  const el = window.EnhancedModal && window.EnhancedModal.createProgressCircle ? window.EnhancedModal.createProgressCircle(percentage) : null;
  if (!el) return null;
  container.appendChild(el);
  return el;
};

// Helpers
function getDefaultColor(idx) {
  const palette = ['#00ffcc','#3aa0ff','#c56cff','#ffaa00','#7ef0a8'];
  return palette[idx % palette.length];
}

function formatNumber(n) {
  if (Number.isFinite(n)) return Math.round(n * 10) / 10;
  return n;
}

// Initialize enhanced components when DOM is ready
if (document.readyState === 'loading') {
  document.addEventListener('DOMContentLoaded', () => {
    window.EnhancedComboBox.init();
  });
} else {
  window.EnhancedComboBox.init();
}

console.log('✨ Enhanced Components v2.1+ Loaded: Combo Box, Modal System, Chart Cards');
