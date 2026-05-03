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

class MainWindow(QWidget):
    SAMPLE_RATE = 44100  
    def __init__(self):
        super().__init__()  
    
        self.setWindowTitle("Audio Interface")

        #Conexión con C
        self.receiver = SocketReceiver()
        self.receiver.batch_received.connect(self.update_buffers_batch)
        self.receiver.start()

        self.pre_buffer = deque(maxlen=16384)
        self.signal_buffer = deque(maxlen=16384)

        self.t = 0
        
        #Definimos el tipo de layer
        self.main_layout = QHBoxLayout()
        self.setLayout(self.main_layout)

        self.left_layout = QVBoxLayout()

        #Titulo
        self.title_label = QLabel("MultiFX Processor")
        self.left_layout.addWidget(self.title_label)
        
        #Presets Dropdown
        self.presets_data = self._load_presets_file()
        self.current_preset_key = next(iter(self.presets_data))

        self.preset_dropdown = QComboBox()
        self.preset_dropdown.addItems(list(self.presets_data.keys()))
        self.preset_dropdown.currentTextChanged.connect(self.on_preset_changed)
        self.left_layout.addWidget(self.preset_dropdown)

        #Effects Dropdown
        self.available_effects = [
            "Overdrive",
            "Distortion",
            "Delay",
            "Wah",
            "Flanger",
            "Chorus",
            "Phaser",
            "PitchShifter",
            "Reverb"
        ]

        #OnClick add effect
        self.add_effect_box = QComboBox()
        self.add_effect_box.addItems(self.available_effects)
        self.add_effect_box.activated.connect(self.add_effect)  
        self.left_layout.addWidget(self.add_effect_box)

        #Lista de efectos
        self.effects_list = QListWidget()
        self.effects_list.setStyleSheet("""
        QListWidget::item:selected {
            background: #dcdcdc;
        }

        QListWidget::item:hover {
            background: #e6e6e6;
        }
        """)
        self.effects_list.setDragDropMode(QListWidget.DragDropMode.InternalMove)
        self.effects_list.model().rowsMoved.connect(self.update_effect_order)
        
        self.model = PresetModel(self.current_preset_key)
        first = self.presets_data[self.current_preset_key]
        self.model.set_effects(first.get("effects", []))
        self.model.master_gain = float(first.get("master_gain", 1.0))


        #Cargar efectos
        self.load_effects()
        self.left_layout.addWidget(self.effects_list)

        # ── Master Gain slider ────────────────────────────────────────────────
        gain_header = QLabel("Master Gain")
        gain_header.setStyleSheet("font-weight: bold; font-size: 11pt;")
        self.left_layout.addWidget(gain_header)

        self.gain_label = QLabel()
        self.left_layout.addWidget(self.gain_label)

        self.gain_slider = QSlider(Qt.Orientation.Horizontal)
        self.gain_slider.setMinimum(10)   # 0.1 * 100 = 10
        self.gain_slider.setMaximum(400)  # 4.0 * 100
        self.gain_slider.setValue(int(self.model.master_gain * 100))
        self._update_gain_label(self.model.master_gain)
        self.gain_slider.valueChanged.connect(self._on_gain_slider_moved)
        self.gain_slider.sliderReleased.connect(self._on_gain_slider_released)
        self.left_layout.addWidget(self.gain_slider)
        
        self.right_layout = QVBoxLayout()

        self.main_layout.addLayout(self.left_layout, 1)
        self.main_layout.addLayout(self.right_layout, 2)
        
        # Estado inicial
        self.show_fft = False

        # Color de los ejes
        label_style = {'color': 'white', 'font-size': '11pt'}
        title_style = {'color': 'white', 'size': '13pt'}

        #Plot de las señales
        self.user_zoom = False
        # Pre
        self.plot_pre = pg.PlotWidget()
        self.plot_pre.setTitle("Pre Effect", **title_style)
        self.plot_pre.setLabel("left", "Amplitude", **label_style)
        self.plot_pre.setLabel("bottom", "Time", **label_style)
        self.plot_pre.getAxis("left").setTextPen('white')
        self.plot_pre.getAxis("bottom").setTextPen('white')
        self.curve_pre = self.plot_pre.plot(
            pen=pg.mkPen(color=(0, 180, 255), width=1.5),
        )
        self.right_layout.addWidget(self.plot_pre)

        # Post
        self.plot_post = pg.PlotWidget()
        self.plot_post.setTitle("Post Effect", **title_style)
        self.plot_post.setLabel("left", "Amplitude", **label_style)
        self.plot_post.setLabel("bottom", "Time", **label_style)
        self.plot_post.getAxis("left").setTextPen('white')
        self.plot_post.getAxis("bottom").setTextPen('white')
        self.curve_post = self.plot_post.plot(
            pen=pg.mkPen(color=(0, 180, 255), width=1.5),
        )
        self.right_layout.addWidget(self.plot_post)

        self.plot_pre.sigRangeChangedManually.connect(lambda: setattr(self, 'user_zoom', True))
        self.plot_post.sigRangeChangedManually.connect(lambda: setattr(self, 'user_zoom', True))

        # Botón toggle
        self.toggle_fft_btn = QPushButton("Show FFT")
        self.toggle_fft_btn.setCheckable(True)
        self.toggle_fft_btn.clicked.connect(self.toggle_fft)
        self.right_layout.addWidget(self.toggle_fft_btn)

        # Bypass button
        self.bypass_active = False
        self.bypass_label = QLabel("BYPASS", self.plot_post)
        self.bypass_label.setStyleSheet("""
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
        self.bypass_label.setCursor(Qt.CursorShape.PointingHandCursor)
        self.bypass_label.adjustSize()
        self.bypass_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.bypass_label.move(self.plot_post.width() - self.bypass_label.width() - 10, 10)
        self.bypass_label.show()
        self.bypass_label.mousePressEvent = self._toggle_bypass_click

        # Pause/Play button
        self.paused = False
        self.pause_label = QLabel("WATCHING", self.plot_pre)
        self.pause_label.setStyleSheet("""
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
        self.pause_label.setCursor(Qt.CursorShape.PointingHandCursor)
        self.pause_label.adjustSize()
        self.pause_label.setFixedWidth(90)
        self.pause_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.pause_label.move(0, 10)  
        self.pause_label.show()
        self.pause_label.mousePressEvent = self._toggle_pause_click
                
        self.timer = QTimer()
        self.timer.timeout.connect(self.sim_signal)
        self.timer.start(100) #Elegir velocidad en la que se generan los puntos

        self.server = TcpServer()
        self.server.json_received.connect(self.handle_remote_json)
        self.server.start()

    def update_buffers_batch(self, pre_batch, post_batch):
        self.pre_buffer.extend(pre_batch)
        self.signal_buffer.extend(post_batch)

    def update_effect_order(self, *args):
        new_order = []

        for i in range(self.effects_list.count()):
            item = self.effects_list.item(i)
            widget = self.effects_list.itemWidget(item)
            new_order.append(widget.effect_data)

        self.model.update_order(new_order)
        self._save_current_preset()
        json_data = self.model.to_json()
        self.receiver.send_json(json_data)
        print(self.model.to_json())

    def handle_remote_json(self, data):
        print("Actualizando desde el celular")

        incoming_name = data.get("name", "Preset Celular")
        MAX_PRESETS = 5

        if incoming_name not in self.presets_data:
            if len(self.presets_data) >= MAX_PRESETS:
                oldest_key = next(iter(self.presets_data))
                print(f"Limite de {MAX_PRESETS} — reemplazando '{oldest_key}'")
                del self.presets_data[oldest_key]

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

        # Sync gain slider
        self.gain_slider.blockSignals(True)
        self.gain_slider.setValue(int(self.model.master_gain * 100))
        self.gain_slider.blockSignals(False)
        self._update_gain_label(self.model.master_gain)

        self._save_current_preset()
        self.load_effects()
        self.receiver.send_json(self.model.to_json())
        print(f"Preset '{incoming_name}' cargado desde celular")
    
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
        # Guardar el preset actual antes de cambiar
        self._save_current_preset()

        # Cargar el nuevo preset
        self.current_preset_key = preset_key
        preset = self.presets_data[preset_key]
        self.model = PresetModel(preset["name"])
        self.model.set_effects(preset.get("effects", []))
        self.model.master_gain = float(preset.get("master_gain", 1.0))

        self.signal_buffer.clear()
        self.pre_buffer.clear()
        self.load_effects()

        # Sync gain slider
        self.gain_slider.blockSignals(True)
        self.gain_slider.setValue(int(self.model.master_gain * 100))
        self.gain_slider.blockSignals(False)
        self._update_gain_label(self.model.master_gain)

        json_data = self.model.to_json()
        self.receiver.send_json(json_data)

    def _save_current_preset(self):
        import json as _json
        # Leer el JSON del modelo actual y guardarlo en el dict en memoria
        raw = self.model.to_json()
        parsed = _json.loads(raw)
        self.presets_data[self.current_preset_key] = {
            "name": parsed["name"],
            "master_gain": parsed.get("master_gain", 1.0),
            "effects": parsed["effects"]
        }
        self._save_presets_file(self.presets_data)

    #Añadir efectos logic
    def add_effect(self):
        if len(self.model.effects) >= 4:
            print("Max 4 effects per parameter")
            return
        effect_type = self.add_effect_box.currentText()

        already_exists = any(e["type"] == effect_type for e in self.model.effects)
        if already_exists:
            print(f"{effect_type} ya esta en la cadena")
            return

        new_id = f"fx_{len(self.model.effects)+1}"

        effect = {
            "id": new_id,
            "type": effect_type,
            "enabled": True,
            "params": self.default_params(effect_type)
        }

        self.model.effects.append(effect)
        self.load_effects()
        self._save_current_preset()

        json_data = self.model.to_json()
        self.receiver.send_json(json_data)

    def remove_effect(self, effect_id):
        print("Removing effect:", effect_id)

        self.model.effects = [
            e for e in self.model.effects if e["id"] != effect_id
        ]

        self.signal_buffer.clear()
        self.pre_buffer.clear()  

        self.load_effects()
        self._save_current_preset()

        json_data = self.model.to_json()
        self.receiver.send_json(json_data)  
    
    #Cargar efectos
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
            "Overdrive": {"GAIN":0.5,"TONE":0.5,"OUTPUT":0.5},
            "Delay": {"TIME":0.5,"FEEDBACK":0.3,"MIX":0.2},
            "Wah": {"FREQ":0.5,"Q":0.8,"LEVEL":1.0},
            "Flanger": {"RATE":0.5,"DEPTH":0.3,"FEEDBACK":0.2,"MIX":0.5},
            "Chorus": {"RATE": 0.5, "DEPTH": 0.3, "FEEDBACK": 0.0, "MIX": 0.2},
            "Phaser": {"RATE":0.5,"DEPTH":0.7,"FEEDBACK":0.3,"MIX":0.5},
            "PitchShifter": {"SEMITONES":0.0, "SEMITONES_B": 0.0, "MIX_A": 1.0, "MIX_B": 0.0, "MIX": 0.5},
            "Reverb": {"FEEDBACK": 0.6, "LPFREQ": 8000.0, "MIX": 0.3},
            "Distortion": {"OUTPUT":0.5},
        }

        return defaults[effect_type]
    
    #Generación del Json
    def generate_json(self):
        print("JSON ready for C: ")
        print(self.model.to_json())

    def update_buffer(self, value):
        self.signal_buffer.append(value)

    def update_pre_buffer(self, value):
        self.pre_buffer.append(value)
        #if len(self.signal_buffer) % 200 == 0:
            #print("post buffer:", len(self.signal_buffer))
    
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
            y = y[-N_FFT:]  # usar los samples más recientes

        y -= np.mean(y)  
        window = np.blackman(N_FFT)
        Y = np.abs(np.fft.rfft(y * window)) * 2.0 / np.sum(window)
        Y_db = 20 * np.log10(Y + 1e-12)

        prev = getattr(self, accum_key, None)
        if prev is None or prev.shape != Y_db.shape:
            setattr(self, accum_key, Y_db)
        else:
            smoothed = 0.7 * prev + 0.3 * Y_db  # α=0.3 
            setattr(self, accum_key, smoothed)

        freqs = np.fft.rfftfreq(N_FFT, d=1.0 / self.SAMPLE_RATE)
        return freqs, getattr(self, accum_key)

    def sim_signal(self):
        pre_data  = np.array(self.pre_buffer)
        post_src  = self.pre_buffer if len(self.model.effects) == 0 else self.signal_buffer
        post_data = np.array(post_src)

        if len(pre_data) > 0:
            pre_data = pre_data - np.mean(pre_data)
        if len(post_data) > 0:
            post_data = post_data - np.mean(post_data)

        x_pre  = np.arange(len(pre_data))
        x_post = np.arange(len(post_data))

        if not self.show_fft:
            # TIME VIEW - Customizable X-axis range
            DISPLAY_SAMPLES = 1024
            pre_display  = pre_data[-DISPLAY_SAMPLES:] if len(pre_data) > DISPLAY_SAMPLES else pre_data
            post_display = post_data[-DISPLAY_SAMPLES:] if len(post_data) > DISPLAY_SAMPLES else post_data

            x_pre  = np.arange(len(pre_display))
            x_post = np.arange(len(post_display))

            self.plot_pre.setLabel("bottom", "Samples")
            self.plot_pre.setLabel("left", "Amplitude")
            self.plot_pre.enableAutoRange()  # Auto Y-axis
            # Unlock X-axis: comment out or modify the next line
            # self.plot_pre.setXRange(0, len(x_pre))  # Remove this to enable auto-scaling
            self.curve_pre.setData(x_pre, pre_display)

            self.plot_post.setLabel("bottom", "Samples")
            self.plot_post.setLabel("left", "Amplitude")
            self.plot_post.enableAutoRange()  # Auto Y-axis
            # self.plot_post.setXRange(0, len(x_post))  # Remove this to enable auto-scaling
            self.curve_post.setData(x_post, post_display)
        else:
            # FFT VIEW - Customizable frequency range
            freqs_pre,  Y_pre  = self._compute_fft(self.pre_buffer, '_fft_pre')
            freqs_post, Y_post = self._compute_fft(post_src,        '_fft_post')
            mask = freqs_pre <= 20000

            if not self.user_zoom:
                # MODIFY THESE RANGES TO YOUR TASTE
                X_MIN_FFT = 0          # Minimum frequency (Hz)
                X_MAX_FFT = 20000      # Maximum frequency (Hz) - change to 10000, 5000, etc.
                Y_MIN_FFT = -170       # Minimum magnitude (dBFS)
                Y_MAX_FFT = 0          # Maximum magnitude (dBFS)
                
                self.plot_pre.setXRange(X_MIN_FFT, X_MAX_FFT)
                self.plot_pre.setYRange(Y_MIN_FFT, Y_MAX_FFT)
                self.plot_post.setXRange(X_MIN_FFT, X_MAX_FFT)
                self.plot_post.setYRange(Y_MIN_FFT, Y_MAX_FFT)

            self.plot_pre.setLabel("bottom", "Frequency (Hz)")
            self.plot_pre.setLabel("left", "Magnitude (dBFS)")
            self.curve_pre.setData(freqs_pre[mask], Y_pre[mask])

            self.plot_post.setLabel("bottom", "Frequency (Hz)")
            self.plot_post.setLabel("left", "Magnitude (dBFS)")
            self.curve_post.setData(freqs_post[mask], Y_post[mask])

    def handle_param_change(self, effect_id, param, value):
        print("MainWindow updating model")

        self.model.update_param(effect_id, param, value)

        json_data = self.model.to_json()

        print("JSON ready for C++:")
        print(json_data)

        self.receiver.send_json(json_data)

    def _update_gain_label(self, gain):
        if gain > 0:
            db = 20.0 * math.log10(gain)
            self.gain_label.setText(f"Master Gain: {gain:.2f}  ({db:+.1f} dB)")
        else:
            self.gain_label.setText("Master Gain: --")

    def _on_gain_slider_moved(self, slider_value):
        gain = slider_value / 100.0
        self._update_gain_label(gain)

    def _on_gain_slider_released(self):
        gain = self.gain_slider.value() / 100.0
        self.model.master_gain = gain
        self._update_gain_label(gain)
        self._save_current_preset()
        self.receiver.send_json(self.model.to_json())
        
    def toggle_fft(self):
        self.show_fft = self.toggle_fft_btn.isChecked()
        if self.show_fft:
            self.toggle_fft_btn.setText("Show Time")
            # Activar fill para FFT
            self.curve_pre.setFillLevel(-200)
            self.curve_post.setFillLevel(-200)
            self.curve_pre.setBrush(pg.mkBrush(0, 140, 255, 60))
            self.curve_post.setBrush(pg.mkBrush(0, 140, 255, 60))
        else:
            self.toggle_fft_btn.setText("Show FFT")
            # Desactivar fill para tiempo
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
            self.bypass_label.setStyleSheet("""
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
        for effect in self.model.effects:
            effect["enabled"] = not self.bypass_active
        self.receiver.send_json(self.model.to_json())

    def _toggle_pause_click(self, event):
            self.paused = not self.paused
            FIXED_WIDTH = 90  
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
                self.pause_label.setStyleSheet("""
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
            self.pause_label.setFixedWidth(FIXED_WIDTH)

    def resizeEvent(self, event):
            super().resizeEvent(event)
            self.bypass_label.move(
                self.plot_post.viewport().width() - self.bypass_label.width() - 10, 10
            )
            self.pause_label.move(
                self.plot_pre.viewport().width() - 90 - 10, 10
            )