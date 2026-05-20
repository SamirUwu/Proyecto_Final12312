import sys
import numpy as np
import pyqtgraph as pg
import json
import os
import math

from ui.effect_widget import EffectWidget
from core.preset_model import PresetModel
from server.receiver_app import TcpServer
from server.receiver_c import SocketReceiver

from collections import deque
from PyQt6.QtWidgets import QApplication, QWidget, QVBoxLayout, QHBoxLayout, QPushButton, QSlider, QLabel, QComboBox, QListWidget, QListWidgetItem 
from PyQt6.QtCore import QTimer, Qt
PRESETS_FILE = "presets.json"

# ── Stylesheets completos para la app ─────────────────────────────────────────
DARK_STYLESHEET = """
    QWidget {
        background-color: #1a1a1a;
        color: #ffffff;
    }
    QComboBox, QListWidget, QSlider {
        background-color: #2a2a2a;
        color: #ffffff;
        border: 1px solid #444;
        border-radius: 4px;
    }
    QComboBox QAbstractItemView {
        background-color: #2a2a2a;
        color: #ffffff;
        selection-background-color: #444;
    }
    QPushButton {
        background-color: #2a2a2a;
        color: #ffffff;
        border: 1px solid #555;
        border-radius: 4px;
        padding: 4px 8px;
    }
    QPushButton:hover { background-color: #3a3a3a; }
    QPushButton:checked { background-color: #004080; border-color: #0066cc; }
    QLabel { background-color: transparent; color: #ffffff; }
    QListWidget::item:selected { background: #3a3a3a; }
    QListWidget::item:hover    { background: #2e2e2e; }
"""

LIGHT_STYLESHEET = """
    QWidget {
        background-color: #f0f0f0;
        color: #000000;
    }
    QComboBox, QListWidget, QSlider {
        background-color: #ffffff;
        color: #000000;
        border: 1px solid #aaa;
        border-radius: 4px;
    }
    QComboBox QAbstractItemView {
        background-color: #ffffff;
        color: #000000;
        selection-background-color: #cce0ff;
    }
    QPushButton {
        background-color: #e0e0e0;
        color: #000000;
        border: 1px solid #aaa;
        border-radius: 4px;
        padding: 4px 8px;
    }
    QPushButton:hover { background-color: #d0d0d0; }
    QPushButton:checked { background-color: #cce0ff; border-color: #4488cc; }
    QLabel { background-color: transparent; color: #000000; }
    QListWidget::item:selected { background: #cce0ff; }
    QListWidget::item:hover    { background: #e8f0ff; }
"""

class MainWindow(QWidget):
    SAMPLE_RATE = 44100  
    def __init__(self):
        super().__init__()  
    
        self.setWindowTitle("Audio Interface")

        # ── Modo claro/oscuro ─────────────────────────────────────────────────
        self.light_mode = False

        #Conexión con C
        self.receiver = SocketReceiver()
        self.receiver.batch_received.connect(self.update_buffers_batch)
        self.receiver.start()

        self.pre_buffer = deque(maxlen=16384)
        self.signal_buffer = deque(maxlen=16384)

        self.t = 0
        
        self.main_layout = QHBoxLayout()
        self.setLayout(self.main_layout)

        self.left_layout = QVBoxLayout()

        self.title_label = QLabel("MultiFX Processor")
        self.left_layout.addWidget(self.title_label)
        
        self.presets_data = self._load_presets_file()
        self.current_preset_key = next(iter(self.presets_data))

        self.preset_dropdown = QComboBox()
        self.preset_dropdown.addItems(list(self.presets_data.keys()))
        self.preset_dropdown.currentTextChanged.connect(self.on_preset_changed)
        self.left_layout.addWidget(self.preset_dropdown)

        self.available_effects = [
            "Overdrive", "Distortion", "Delay", "Wah",
            "Flanger", "Chorus", "Phaser", "PitchShifter", "Reverb"
        ]

        self.add_effect_box = QComboBox()
        self.add_effect_box.addItems(self.available_effects)
        self.add_effect_box.activated.connect(self.add_effect)  
        self.left_layout.addWidget(self.add_effect_box)

        self.effects_list = QListWidget()
        self.effects_list.setDragDropMode(QListWidget.DragDropMode.InternalMove)
        self.effects_list.model().rowsMoved.connect(self.update_effect_order)
        
        self.model = PresetModel(self.current_preset_key)
        first = self.presets_data[self.current_preset_key]
        self.model.set_effects(first.get("effects", []))
        self.model.master_gain = float(first.get("master_gain", 1.0))

        self.load_effects()
        self.left_layout.addWidget(self.effects_list)

        gain_header = QLabel("Master Gain")
        gain_header.setStyleSheet("font-weight: bold; font-size: 11pt;")
        self.left_layout.addWidget(gain_header)

        self.gain_label = QLabel()
        self.left_layout.addWidget(self.gain_label)

        self.gain_slider = QSlider(Qt.Orientation.Horizontal)
        self.gain_slider.setMinimum(10)
        self.gain_slider.setMaximum(400)
        self.gain_slider.setValue(int(self.model.master_gain * 100))
        self._update_gain_label(self.model.master_gain)
        self.gain_slider.valueChanged.connect(self._on_gain_slider_moved)
        self.gain_slider.sliderReleased.connect(self._on_gain_slider_released)
        self.left_layout.addWidget(self.gain_slider)
        
        self.right_layout = QVBoxLayout()
        self.main_layout.addLayout(self.left_layout, 1)
        self.main_layout.addLayout(self.right_layout, 2)
        
        self.show_fft = False

        # ── Colores de los plots por tema ─────────────────────────────────────
        self._dark = {
            'bg':        (20, 20, 20),
            'curve':     (0, 180, 255),
            'axis_text': 'white',
            'title':     {'color': 'white', 'size': '13pt'},
            'label':     {'color': 'white', 'font-size': '11pt'},
        }
        self._light = {
            'bg':        (255, 255, 255),
            'curve':     (0, 100, 200),
            'axis_text': 'black',
            'title':     {'color': 'black', 'size': '13pt'},
            'label':     {'color': 'black', 'font-size': '11pt'},
        }

        # ── Plot Pre ──────────────────────────────────────────────────────────
        self.user_zoom = False
        self.plot_pre = pg.PlotWidget()
        self.plot_pre.setBackground((20, 20, 20))
        self.plot_pre.setTitle("Pre Effect", color='white', size='13pt')
        self.plot_pre.setLabel("left", "Amplitude", color='white', **{'font-size': '11pt'})
        self.plot_pre.setLabel("bottom", "Time", color='white', **{'font-size': '11pt'})
        self.plot_pre.getAxis("left").setTextPen('white')
        self.plot_pre.getAxis("bottom").setTextPen('white')
        self.curve_pre = self.plot_pre.plot(pen=pg.mkPen(color=(0, 180, 255), width=1.5))
        self.right_layout.addWidget(self.plot_pre)

        # ── Plot Post ─────────────────────────────────────────────────────────
        self.plot_post = pg.PlotWidget()
        self.plot_post.setBackground((20, 20, 20))
        self.plot_post.setTitle("Post Effect", color='white', size='13pt')
        self.plot_post.setLabel("left", "Amplitude", color='white', **{'font-size': '11pt'})
        self.plot_post.setLabel("bottom", "Time", color='white', **{'font-size': '11pt'})
        self.plot_post.getAxis("left").setTextPen('white')
        self.plot_post.getAxis("bottom").setTextPen('white')
        self.curve_post = self.plot_post.plot(pen=pg.mkPen(color=(0, 180, 255), width=1.5))
        self.right_layout.addWidget(self.plot_post)

        self.plot_pre.sigRangeChangedManually.connect(lambda: setattr(self, 'user_zoom', True))
        self.plot_post.sigRangeChangedManually.connect(lambda: setattr(self, 'user_zoom', True))

        self.toggle_fft_btn = QPushButton("Show FFT")
        self.toggle_fft_btn.setCheckable(True)
        self.toggle_fft_btn.clicked.connect(self.toggle_fft)
        self.right_layout.addWidget(self.toggle_fft_btn)

        # ── BYPASS button (plot_post) ─────────────────────────────────────────
        self.bypass_active = False
        self.bypass_label = QLabel("BYPASS", self.plot_post)
        self._style_overlay_btn(self.bypass_label, dark=True)
        self.bypass_label.setCursor(Qt.CursorShape.PointingHandCursor)
        self.bypass_label.adjustSize()
        self.bypass_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.bypass_label.show()
        self.bypass_label.mousePressEvent = self._toggle_bypass_click

        # ── WATCHING + LIGHT MODE buttons (plot_pre, misma fila derecha) ──────
        self.paused = False

        # WATCHING
        self.pause_label = QLabel("WATCHING", self.plot_pre)
        self._style_overlay_btn(self.pause_label, dark=True)
        self.pause_label.setCursor(Qt.CursorShape.PointingHandCursor)
        self.pause_label.setFixedWidth(90)
        self.pause_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.pause_label.show()
        self.pause_label.mousePressEvent = self._toggle_pause_click

        # LIGHT MODE — mismo ancho fijo para alinear
        self.theme_label = QLabel("☀ LIGHT MODE", self.plot_pre)
        self._style_overlay_btn(self.theme_label, dark=True)
        self.theme_label.setCursor(Qt.CursorShape.PointingHandCursor)
        self.theme_label.setFixedWidth(90)
        self.theme_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.theme_label.show()
        self.theme_label.mousePressEvent = self._toggle_theme_click

        self.timer = QTimer()
        self.timer.timeout.connect(self.sim_signal)
        self.timer.start(250)

        self.server = TcpServer()
        self.server.json_received.connect(self.handle_remote_json)
        self.server.start()

        # Aplicar tema oscuro inicial
        QApplication.instance().setStyleSheet(DARK_STYLESHEET)

    # ── Overlay button style helper ───────────────────────────────────────────
    def _style_overlay_btn(self, label, dark=True):
        if dark:
            label.setStyleSheet("""
                QLabel {
                    color: #aaaaaa;
                    background-color: rgba(0,0,0,180);
                    border: 1px solid #555;
                    border-radius: 4px;
                    padding: 3px 8px;
                    font-size: 11px;
                    font-weight: bold;
                }
            """)
        else:
            label.setStyleSheet("""
                QLabel {
                    color: #333333;
                    background-color: rgba(220,220,220,210);
                    border: 1px solid #999;
                    border-radius: 4px;
                    padding: 3px 8px;
                    font-size: 11px;
                    font-weight: bold;
                }
            """)

    # ── Posicionar los dos botones apilados en esquina superior derecha ───────
    def _reposition_pre_buttons(self):
        vw = self.plot_pre.viewport().width()
        margin = 6
        # WATCHING arriba
        self.pause_label.move(vw - 90 - margin, margin)
        # LIGHT MODE justo debajo
        self.pause_label.adjustSize()
        h = self.pause_label.height()
        self.theme_label.move(vw - 90 - margin, margin + h + 4)

    # ── Aplicar tema completo ─────────────────────────────────────────────────
    def _apply_theme(self):
        t = self._light if self.light_mode else self._dark
        is_light = self.light_mode

        # App completa
        QApplication.instance().setStyleSheet(LIGHT_STYLESHEET if is_light else DARK_STYLESHEET)

        # Plots
        for plot in [self.plot_pre, self.plot_post]:
            plot.setBackground(t['bg'])
            plot.getAxis("left").setTextPen(t['axis_text'])
            plot.getAxis("bottom").setTextPen(t['axis_text'])
            plot.getAxis("left").setPen(t['axis_text'])
            plot.getAxis("bottom").setPen(t['axis_text'])

        self.plot_pre.setTitle("Pre Effect",   **t['title'])
        self.plot_post.setTitle("Post Effect", **t['title'])
        for plot in [self.plot_pre, self.plot_post]:
            plot.setLabel("left",   "Amplitude", **t['label'])
            plot.setLabel("bottom", "Samples",   **t['label'])

        curve_pen = pg.mkPen(color=t['curve'], width=1.5)
        self.curve_pre.setPen(curve_pen)
        self.curve_post.setPen(curve_pen)

        if self.show_fft:
            brush = pg.mkBrush(*t['curve'], 60)
            self.curve_pre.setBrush(brush)
            self.curve_post.setBrush(brush)

        # Overlay buttons
        self._style_overlay_btn(self.pause_label, dark=not is_light)
        self._style_overlay_btn(self.bypass_label, dark=not is_light)

        if is_light:
            self.theme_label.setText("🌙 DARK MODE")
            self._style_overlay_btn(self.theme_label, dark=False)
        else:
            self.theme_label.setText("☀ LIGHT MODE")
            self._style_overlay_btn(self.theme_label, dark=True)

        self._reposition_pre_buttons()

    def _toggle_theme_click(self, event):
        self.light_mode = not self.light_mode
        self._apply_theme()

    # ── Resto de métodos ──────────────────────────────────────────────────────

    def update_effect_order(self, *args):
        new_order = []
        for i in range(self.effects_list.count()):
            item = self.effects_list.item(i)
            widget = self.effects_list.itemWidget(item)
            new_order.append(widget.effect_data)
        self.model.update_order(new_order)
        self._save_current_preset()
        self.receiver.send_json(self.model.to_json())

    def handle_remote_json(self, data):
        incoming_name = data.get("name", "Preset Celular")
        MAX_PRESETS = 5
        if incoming_name not in self.presets_data:
            if len(self.presets_data) >= MAX_PRESETS:
                del self.presets_data[next(iter(self.presets_data))]
            self.presets_data[incoming_name] = {"name": incoming_name, "effects": []}
            self.preset_dropdown.blockSignals(True)
            self.preset_dropdown.clear()
            self.preset_dropdown.addItems(list(self.presets_data.keys()))
            self.preset_dropdown.blockSignals(False)
        self.current_preset_key = incoming_name
        self.model = PresetModel(incoming_name)
        self.model.load_from_json(data)
        self.preset_dropdown.blockSignals(True)
        self.preset_dropdown.setCurrentText(incoming_name)
        self.preset_dropdown.blockSignals(False)
        self.gain_slider.blockSignals(True)
        self.gain_slider.setValue(int(self.model.master_gain * 100))
        self.gain_slider.blockSignals(False)
        self._update_gain_label(self.model.master_gain)
        self._save_current_preset()
        self.load_effects()
        self.receiver.send_json(self.model.to_json())

    def _load_presets_file(self):
        if not os.path.exists(PRESETS_FILE):
            default = {
                "Preset 1": {"name": "Preset1", "effects": []},
                "Preset 2": {"name": "Preset2", "effects": []},
                "Preset 3": {"name": "Preset3", "effects": []},
            }
            with open(PRESETS_FILE, "w") as f:
                json.dump(default, f, indent=2)
        with open(PRESETS_FILE, "r") as f:
            return json.load(f)

    def _save_presets_file(self, presets_data):
        with open(PRESETS_FILE, "w") as f:
            json.dump(presets_data, f, indent=2)

    def on_preset_changed(self, preset_key):
        self._save_current_preset()
        self.current_preset_key = preset_key
        preset = self.presets_data[preset_key]
        self.model = PresetModel(preset["name"])
        self.model.set_effects(preset.get("effects", []))
        self.model.master_gain = float(preset.get("master_gain", 1.0))
        self.signal_buffer.clear()
        self.pre_buffer.clear()
        self.load_effects()
        self.gain_slider.blockSignals(True)
        self.gain_slider.setValue(int(self.model.master_gain * 100))
        self.gain_slider.blockSignals(False)
        self._update_gain_label(self.model.master_gain)
        self.receiver.send_json(self.model.to_json())

    def _save_current_preset(self):
        import json as _json
        raw = self.model.to_json()
        parsed = _json.loads(raw)
        self.presets_data[self.current_preset_key] = {
            "name": parsed["name"],
            "master_gain": parsed.get("master_gain", 1.0),
            "effects": parsed["effects"]
        }
        self._save_presets_file(self.presets_data)

    def add_effect(self):
        if len(self.model.effects) >= 4:
            return
        effect_type = self.add_effect_box.currentText()
        if any(e["type"] == effect_type for e in self.model.effects):
            return
        effect = {
            "id": f"fx_{len(self.model.effects)+1}",
            "type": effect_type,
            "enabled": True,
            "params": self.default_params(effect_type)
        }
        self.model.effects.append(effect)
        self.load_effects()
        self._save_current_preset()
        self.receiver.send_json(self.model.to_json())

    def remove_effect(self, effect_id):
        self.model.effects = [e for e in self.model.effects if e["id"] != effect_id]
        self.signal_buffer.clear()
        self.pre_buffer.clear()
        self.load_effects()
        self._save_current_preset()
        self.receiver.send_json(self.model.to_json())

    def load_effects(self):
        self.effects_list.clear()
        for effect in self.model.effects:
            item = QListWidgetItem()
            widget = EffectWidget(effect)
            widget.list_item = item
            item.setSizeHint(widget.sizeHint())
            widget.param_changed.connect(self.handle_param_change)
            widget.delete_requested.connect(self.remove_effect)
            self.effects_list.addItem(item)
            self.effects_list.setItemWidget(item, widget)

    def default_params(self, effect_type):
        defaults = {
            "Overdrive":    {"GAIN": 0.5, "TONE": 0.5, "OUTPUT": 0.5},
            "Delay":        {"TIME": 0.5, "FEEDBACK": 0.3, "MIX": 0.2},
            "Wah":          {"FREQ": 0.5, "Q": 0.8, "LEVEL": 1.0},
            "Flanger":      {"RATE": 0.5, "DEPTH": 0.3, "FEEDBACK": 0.2, "MIX": 0.5},
            "Chorus":       {"RATE": 0.5, "DEPTH": 0.3, "FEEDBACK": 0.0, "MIX": 0.2},
            "Phaser":       {"RATE": 0.5, "DEPTH": 0.7, "FEEDBACK": 0.3, "MIX": 0.5},
            "PitchShifter": {"SEMITONES": 0.0, "SEMITONES_B": 0.0, "MIX_A": 1.0, "MIX_B": 0.0, "MIX": 0.5},
            "Reverb":       {"FEEDBACK": 0.6, "LPFREQ": 8000.0, "MIX": 0.3},
            "Distortion":   {"OUTPUT": 0.5},
        }
        return defaults[effect_type]

    def update_buffers_batch(self, pre_batch, post_batch):
        VREF = 3.3
        pre_volts  = [(x + 1.0) * (VREF / 2.0) for x in pre_batch]
        post_volts = [(x + 1.0) * (VREF / 2.0) for x in post_batch]
        self.pre_buffer.extend(pre_volts)
        self.signal_buffer.extend(post_volts)

    def _compute_fft(self, buffer, accum_key):
        N_FFT = 2048
        y = np.array(buffer, dtype=float)
        if len(y) < N_FFT:
            y = np.pad(y, (0, N_FFT - len(y)), 'constant')
        else:
            y = y[-N_FFT:]
        y -= np.mean(y)
        window = np.blackman(N_FFT)
        Y = np.abs(np.fft.rfft(y * window)) * 2.0 / np.sum(window)
        Y_db = 20 * np.log10(Y + 1e-12)
        prev = getattr(self, accum_key, None)
        if prev is None or prev.shape != Y_db.shape:
            setattr(self, accum_key, Y_db)
        else:
            setattr(self, accum_key, 0.7 * prev + 0.3 * Y_db)
        freqs = np.fft.rfftfreq(N_FFT, d=1.0 / self.SAMPLE_RATE)
        return freqs, getattr(self, accum_key)

    def sim_signal(self):
        pre_data  = np.array(self.pre_buffer)
        post_src  = self.pre_buffer if len(self.model.effects) == 0 else self.signal_buffer
        post_data = np.array(post_src)
        if len(pre_data)  > 0: pre_data  = pre_data  - np.mean(pre_data)
        if len(post_data) > 0: post_data = post_data - np.mean(post_data)

        if not self.show_fft:
            DISPLAY_SAMPLES = 1024
            pre_display  = pre_data[-DISPLAY_SAMPLES:]  if len(pre_data)  > DISPLAY_SAMPLES else pre_data
            post_display = post_data[-DISPLAY_SAMPLES:] if len(post_data) > DISPLAY_SAMPLES else post_data
            self.plot_pre.setLabel("bottom", "Samples")
            self.plot_pre.setLabel("left", "Amplitude")
            self.plot_pre.enableAutoRange()
            self.curve_pre.setData(np.arange(len(pre_display)), pre_display)
            self.plot_post.setLabel("bottom", "Samples")
            self.plot_post.setLabel("left", "Amplitude")
            self.plot_post.enableAutoRange()
            self.curve_post.setData(np.arange(len(post_display)), post_display)
        else:
            freqs_pre,  Y_pre  = self._compute_fft(self.pre_buffer, '_fft_pre')
            freqs_post, Y_post = self._compute_fft(post_src,        '_fft_post')
            mask = freqs_pre <= 20000
            if not self.user_zoom:
                for p in [self.plot_pre, self.plot_post]:
                    p.setXRange(0, 20000)
                    p.setYRange(-170, 0)
            self.plot_pre.setLabel("bottom", "Frequency (Hz)")
            self.plot_pre.setLabel("left", "Magnitude (dBFS)")
            self.curve_pre.setData(freqs_pre[mask], Y_pre[mask])
            self.plot_post.setLabel("bottom", "Frequency (Hz)")
            self.plot_post.setLabel("left", "Magnitude (dBFS)")
            self.curve_post.setData(freqs_post[mask], Y_post[mask])

    def handle_param_change(self, effect_id, param, value):
        self.model.update_param(effect_id, param, value)
        self.receiver.send_json(self.model.to_json())

    def _update_gain_label(self, gain):
        if gain > 0:
            db = 20.0 * math.log10(gain)
            self.gain_label.setText(f"Master Gain: {gain:.2f}  ({db:+.1f} dB)")
        else:
            self.gain_label.setText("Master Gain: --")

    def _on_gain_slider_moved(self, slider_value):
        self._update_gain_label(slider_value / 100.0)

    def _on_gain_slider_released(self):
        gain = self.gain_slider.value() / 100.0
        self.model.master_gain = gain
        self._update_gain_label(gain)
        self._save_current_preset()
        self.receiver.send_json(self.model.to_json())

    def toggle_fft(self):
        self.show_fft = self.toggle_fft_btn.isChecked()
        t = self._light if self.light_mode else self._dark
        if self.show_fft:
            self.toggle_fft_btn.setText("Show Time")
            brush = pg.mkBrush(*t['curve'], 60)
            self.curve_pre.setFillLevel(-200)
            self.curve_post.setFillLevel(-200)
            self.curve_pre.setBrush(brush)
            self.curve_post.setBrush(brush)
        else:
            self.toggle_fft_btn.setText("Show FFT")
            self.curve_pre.setFillLevel(None)
            self.curve_post.setFillLevel(None)
            self.curve_pre.setBrush(None)
            self.curve_post.setBrush(None)

    def _toggle_bypass_click(self, event):
        self.bypass_active = not self.bypass_active
        if self.bypass_active:
            self.bypass_label.setStyleSheet("""
                QLabel {
                    color: #ff4444;
                    background-color: rgba(80,0,0,200);
                    border: 1px solid #ff4444;
                    border-radius: 4px;
                    padding: 3px 8px;
                    font-size: 11px;
                    font-weight: bold;
                }
            """)
        else:
            self._style_overlay_btn(self.bypass_label, dark=not self.light_mode)
        for effect in self.model.effects:
            effect["enabled"] = not self.bypass_active
        self.receiver.send_json(self.model.to_json())

    def _toggle_pause_click(self, event):
        self.paused = not self.paused
        if self.paused:
            self.timer.stop()
            self.pause_label.setText("STOPPED")
            self.pause_label.setStyleSheet("""
                QLabel {
                    color: #ffaa00;
                    background-color: rgba(60,40,0,200);
                    border: 1px solid #ffaa00;
                    border-radius: 4px;
                    padding: 3px 8px;
                    font-size: 11px;
                    font-weight: bold;
                }
            """)
        else:
            self.timer.start(100)
            self.pause_label.setText("WATCHING")
            self._style_overlay_btn(self.pause_label, dark=not self.light_mode)
        self.pause_label.setFixedWidth(90)

    def resizeEvent(self, event):
        super().resizeEvent(event)
        # BYPASS — esquina superior derecha de plot_post
        self.bypass_label.move(
            self.plot_post.viewport().width() - self.bypass_label.width() - 6, 6
        )
        # WATCHING y LIGHT MODE — apilados en esquina superior derecha de plot_pre
        self._reposition_pre_buttons()