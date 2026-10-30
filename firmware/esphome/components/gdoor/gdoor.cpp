/*
 * This file is part of the GDoor distribution (https://github.com/gdoor-org).
 * Copyright (c) 2024 GDoor authors.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */
#include "gdoor.h"

#include <cctype>
#include <cstring>
#include <esp_timer.h>

#include "esphome/core/log.h"

namespace esphome {
namespace gdoor {

static const char *const TAG = "gdoor";

// The bus modulates bits with a ~60kHz carrier, the bit value is the length of the pulse train.
// RX: Detect end of bit / message by the time since the last carrier edge
static const int64_t BIT_GAP_US = 167;       // 10 carrier periods without edge: bit is over
static const int64_t MESSAGE_GAP_US = 2250;  // 3 * STARTBIT_MIN_LEN carrier periods without edge: message is over
// RX: Decoding, in carrier edges per bit
static const float BIT_ONE_DIV = 2.5f;       // Threshold between 1/0 bit, relative to the start bit length
static const uint16_t BIT_MIN_LEN = 5;       // Shorter pulse trains are ignored as noise
static const uint16_t STARTBIT_MIN_LEN = 45;
// TX: in 60kHz carrier periods
static const uint32_t STARTBIT_PULSENUM = 66;
static const uint32_t ONE_PULSENUM = 16;
static const uint32_t ZERO_PULSENUM = 37;
static const uint32_t PAUSE_PULSENUM = 30;
// We only modulate with 52kHz, as the bandpass manufacturing tolerances are a bit on the lower side.
static const uint32_t TX_CARRIER_HZ = 52000;
static const uint32_t TX_RESOLUTION_HZ = 1000000;
static const size_t TX_QUEUE_MAX = 5;

static uint8_t crc(const uint8_t *data, uint16_t len) {
  uint8_t crc = 0;
  for (uint16_t i = 0; i < len; i++) {
    crc = crc + data[i];
  }
  return crc;
}

static uint8_t parity_odd(uint8_t word) { return __builtin_popcount(word) & 0x01; }

static std::string to_hex(const uint8_t *data, size_t len) {
  static const char *const HEX_CHARS = "0123456789ABCDEF";
  std::string str;
  str.reserve(len * 2);
  for (size_t i = 0; i < len; i++) {
    str += HEX_CHARS[data[i] >> 4];
    str += HEX_CHARS[data[i] & 0x0F];
  }
  return str;
}

static const char *hwtype_name(uint8_t value) {
  switch (value) {
    case 0xA0: return "OUTDOOR";
    case 0xA1: return "INDOOR";
    case 0xA2: return "INDOOR_RECEIVER";
    case 0xA3: return "CONTROLLER";
    case 0xA4: return "ACTUATOR";
    case 0xA5: return "GATEWAY_TK";
    case 0xA6: return "CHIME";
    case 0xA7: return "BUTTON_IF";
    case 0xA8: return "GATEWAY_IP";
    default: return "TYPE_UNKNOWN";
  }
}

static const char *action_name(uint8_t value) {
  switch (value) {
    case 0x42: return "BUTTON";
    case 0x41: return "BUTTON_LIGHT";
    case 0x31: return "DOOR_OPEN";
    case 0x28: return "VIDEO_REQUEST";
    case 0x21: return "AUDIO_REQUEST";
    case 0x20: return "AUDIO_VIDEO_END";
    case 0x13: return "BUTTON_FLOOR";
    case 0x12: return "CALL_INTERNAL";
    case 0x11: return "BUTTON_RING";
    case 0x0F: return "CTRL_DOOROPENER_ACK";
    case 0x08: return "CTRL_RESET";
    case 0x05: return "CTRL_DOORSTATION_ACK";
    case 0x04: return "CTRL_BUTTONS_TRAINING_START";
    case 0x03: return "CTRL_DOOROPENER_TRAINING_START";
    case 0x02: return "CTRL_DOOROPENER_TRAINING_STOP";
    case 0x01: return "CTRL_PROGRAMMING_START";
    case 0x00: return "CTRL_PROGRAMMING_STOP";
    default: return "ACTION_UNKNOWN";
  }
}

void GDoorComponent::setup() {
  if (this->rx_threshold_output_ != nullptr) {
    this->rx_threshold_output_->set_level(this->rx_threshold_voltage_ / 3.3f);
  }

  this->tx_enable_pin_->setup();
  this->tx_enable_pin_->digital_write(false);
  this->tx_enable_isr_pin_ = this->tx_enable_pin_->to_isr();

  rmt_tx_channel_config_t channel_config{};
  channel_config.gpio_num = (gpio_num_t) this->tx_pin_->get_pin();
  channel_config.clk_src = RMT_CLK_SRC_DEFAULT;
  channel_config.resolution_hz = TX_RESOLUTION_HZ;
  channel_config.mem_block_symbols = 64;
  channel_config.trans_queue_depth = 1;
  esp_err_t err = rmt_new_tx_channel(&channel_config, &this->tx_channel_);
  if (err == ESP_OK) {
    rmt_carrier_config_t carrier{};
    carrier.frequency_hz = TX_CARRIER_HZ;
    carrier.duty_cycle = 0.5f;
    err = rmt_apply_carrier(this->tx_channel_, &carrier);
  }
  if (err == ESP_OK) {
    rmt_copy_encoder_config_t encoder_config{};
    err = rmt_new_copy_encoder(&encoder_config, &this->tx_encoder_);
  }
  if (err == ESP_OK) {
    rmt_tx_event_callbacks_t callbacks{};
    callbacks.on_trans_done = GDoorComponent::tx_done_isr;
    err = rmt_tx_register_event_callbacks(this->tx_channel_, &callbacks, this);
  }
  if (err == ESP_OK) {
    err = rmt_enable(this->tx_channel_);
  }
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Setting up RMT for TX failed: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }

  this->rx_pin_->setup();
  this->rx_pin_->attach_interrupt(GDoorComponent::rx_edge_isr, this, gpio::INTERRUPT_FALLING_EDGE);
}

void GDoorComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "GDoor:");
  LOG_PIN("  RX Pin: ", this->rx_pin_);
  LOG_PIN("  TX Pin: ", this->tx_pin_);
  LOG_PIN("  TX Enable Pin: ", this->tx_enable_pin_);
  if (this->rx_threshold_output_ != nullptr) {
    ESP_LOGCONFIG(TAG, "  RX Threshold: %.2fV", this->rx_threshold_voltage_);
  }
}

/*
 * Called on every falling edge of the received carrier.
 * Counts the edges per bit, a gap without edges ends a bit or the whole message.
 */
void IRAM_ATTR GDoorComponent::rx_edge_isr(GDoorComponent *arg) {
  int64_t now = esp_timer_get_time();
  portENTER_CRITICAL_ISR(&arg->lock_);
  if (!arg->tx_active_) {
    if (arg->pulse_count_ > 0) {
      int64_t gap = now - arg->last_edge_us_;
      if (gap > MESSAGE_GAP_US) {
        arg->finish_message_();  // loop() did not fetch the previous message yet
      } else if (gap > BIT_GAP_US) {
        arg->finish_bit_();
      }
    }
    arg->pulse_count_ = arg->pulse_count_ + 1;
    arg->last_edge_us_ = now;
  }
  portEXIT_CRITICAL_ISR(&arg->lock_);
}

/// Store the edges of the current bit. Needs lock_.
void IRAM_ATTR GDoorComponent::finish_bit_() {
  if (this->bit_count_ < MAX_BITS) {
    this->counts_[this->bit_count_] = this->pulse_count_;
    this->bit_count_ = this->bit_count_ + 1;
  }
  this->pulse_count_ = 0;
}

/// Hand the current message over to loop(). Needs lock_.
void IRAM_ATTR GDoorComponent::finish_message_() {
  this->finish_bit_();
  if (this->message_ready_) {
    this->messages_dropped_ = this->messages_dropped_ + 1;
  } else {
    memcpy(this->ready_counts_, this->counts_, this->bit_count_ * sizeof(uint16_t));
    this->ready_len_ = this->bit_count_;
    this->message_ready_ = true;
  }
  this->bit_count_ = 0;
}

/*
 * Called by the RMT driver when sending is done.
 * Disables the output buffer and enables RX again.
 */
bool IRAM_ATTR GDoorComponent::tx_done_isr(rmt_channel_handle_t channel, const rmt_tx_done_event_data_t *event,
                                           void *arg) {
  auto *self = static_cast<GDoorComponent *>(arg);
  self->tx_enable_isr_pin_.digital_write(false);
  portENTER_CRITICAL_ISR(&self->lock_);
  self->pulse_count_ = 0;
  self->bit_count_ = 0;
  self->tx_active_ = false;
  portEXIT_CRITICAL_ISR(&self->lock_);
  return false;
}

void GDoorComponent::loop() {
  bool ready = false;
  uint16_t len = 0;
  uint32_t dropped = 0;
  bool bus_busy;

  portENTER_CRITICAL(&this->lock_);
  if (this->pulse_count_ > 0 && esp_timer_get_time() - this->last_edge_us_ > MESSAGE_GAP_US) {
    this->finish_message_();
  }
  if (this->message_ready_) {
    ready = true;
    len = this->ready_len_;
    memcpy(this->decode_counts_, this->ready_counts_, len * sizeof(uint16_t));
    this->message_ready_ = false;
  }
  dropped = this->messages_dropped_;
  this->messages_dropped_ = 0;
  bus_busy = this->pulse_count_ > 0 || this->tx_active_;
  portEXIT_CRITICAL(&this->lock_);

  if (dropped > 0) {
    ESP_LOGW(TAG, "%u bus message(s) dropped, loop() too slow", (unsigned) dropped);
  }

  if (ready) {
    GDoorMessage message;
    if (this->decode_(this->decode_counts_, len, message)) {
      if (message.valid) {
        ESP_LOGI(TAG, "Received %s: action=%s type=%s source=%s destination=%s parameters=%s",
                 message.busdata.c_str(), message.action.c_str(), message.type.c_str(), message.source.c_str(),
                 message.destination.c_str(), message.parameters.c_str());
        this->message_callback_.call(message);
      } else {
        ESP_LOGW(TAG, "Received invalid message (parity/checksum error): %s", message.busdata.c_str());
      }
    } else {
      ESP_LOGD(TAG, "Received %u pulse trains, but no message", len);
    }
#if ESPHOME_LOG_LEVEL >= ESPHOME_LOG_LEVEL_VERBOSE
    std::string counts;
    for (uint16_t i = 0; i < len; i++) {
      counts += to_string(this->decode_counts_[i]) + " ";
    }
    ESP_LOGV(TAG, "Edges per bit: %s", counts.c_str());
#endif
  }

  // Only send while the bus is idle, otherwise we would disturb other participants
  if (!bus_busy && !this->tx_queue_.empty()) {
    this->start_transmission_(this->tx_queue_.front());
    this->tx_queue_.pop_front();
  }
}

/**
 * Decode the carrier edges per bit into bytes and the high level protocol.
 * @return false if not a single word was received
 */
bool GDoorComponent::decode_(const uint16_t *counts, uint16_t len, GDoorMessage &message) {
  uint8_t data[MAX_WORDLEN];
  uint16_t wordcounter = 0;
  uint8_t bitindex = 0;   // Bit inside the current word, 0 to 8 (8 data bits + parity)
  bool is_startbit = true;
  uint16_t bit_one_thres = 0;  // Dynamic 1/0 threshold, based on length of the start bit
  bool valid = true;

  for (uint16_t i = 0; i < len && wordcounter < MAX_WORDLEN; i++) {
    uint16_t cnt = counts[i];

    // Filter out smaller pulses, just ignore them
    if (cnt < BIT_MIN_LEN) {
      continue;
    }

    // First bit is the start bit, it determines the length of one and zero bits
    if (is_startbit) {
      if (cnt < STARTBIT_MIN_LEN) {
        continue;
      }
      bit_one_thres = cnt / BIT_ONE_DIV;
      is_startbit = false;
      continue;
    }

    if (bitindex == 0) {
      data[wordcounter] = 0;
    }
    uint8_t bit = cnt < bit_one_thres ? 1 : 0;

    if (bitindex == 8) {  // Parity bit
      if (parity_odd(data[wordcounter]) != bit) {
        valid = false;
      }
      bitindex = 0;
      wordcounter++;
    } else {  // Data bits, LSB first
      data[wordcounter] |= (uint8_t) (bit << bitindex);
      bitindex++;
    }
  }

  if (wordcounter == 0) {
    return false;
  }

  // Last word is the checksum
  if (crc(data, wordcounter - 1) != data[wordcounter - 1]) {
    valid = false;
  }

  uint8_t none[3] = {0, 0, 0};
  message.busdata = to_hex(data, wordcounter);
  message.valid = valid;
  message.action = "ACTION_UNKNOWN";
  message.type = "TYPE_UNKNOWN";
  message.source = to_hex(none, 3);
  message.destination = to_hex(none, 3);
  message.parameters = to_hex(none, 2);

  if (valid && wordcounter >= 9) {
    message.action = action_name(data[2]);
    message.source = to_hex(&data[3], 3);
    message.parameters = to_hex(&data[6], 2);
    message.type = hwtype_name(data[8]);
    if (wordcounter >= 12) {
      message.destination = to_hex(&data[9], 3);
    }
  }
  return true;
}

bool GDoorComponent::send(const std::string &busdata) {
  std::string hex;
  for (char c : busdata) {
    if (c == ' ' || c == ':') {
      continue;
    }
    if (!std::isxdigit((unsigned char) c)) {
      ESP_LOGW(TAG, "Not sending '%s', invalid hex character", busdata.c_str());
      return false;
    }
    hex += c;
  }
  if (hex.empty() || hex.size() % 2 != 0 || hex.size() / 2 >= MAX_WORDLEN) {
    ESP_LOGW(TAG, "Not sending '%s', needs 1 to %u bytes as hex", busdata.c_str(), (unsigned) (MAX_WORDLEN - 1));
    return false;
  }
  std::vector<uint8_t> data;
  for (size_t i = 0; i < hex.size(); i += 2) {
    data.push_back((uint8_t) strtoul(hex.substr(i, 2).c_str(), nullptr, 16));
  }

  if (this->tx_queue_.size() >= TX_QUEUE_MAX) {
    ESP_LOGW(TAG, "Not sending '%s', send queue is full", busdata.c_str());
    return false;
  }
  this->tx_queue_.push_back(std::move(data));
  return true;
}

void GDoorComponent::start_transmission_(const std::vector<uint8_t> &data) {
  // On time is one carrier period longer than the pulse number, like the original timer based implementation
  auto pulse_us = [](uint32_t pulses) { return (uint16_t) ((pulses + 1) * TX_RESOLUTION_HZ / 60000); };

  uint16_t num_symbols = 0;
  auto add_bit = [&](uint32_t pulses) {
    rmt_symbol_word_t &symbol = this->tx_symbols_[num_symbols++];
    symbol.level0 = 1;  // Carrier on
    symbol.duration0 = pulse_us(pulses);
    symbol.level1 = 0;  // Pause
    symbol.duration1 = pulse_us(PAUSE_PULSENUM);
  };

  add_bit(STARTBIT_PULSENUM);
  std::vector<uint8_t> words = data;
  words.push_back(crc(data.data(), data.size()));
  for (uint8_t byte : words) {
    for (uint8_t i = 0; i < 8; i++) {  // LSB first
      add_bit((byte >> i) & 0x01 ? ONE_PULSENUM : ZERO_PULSENUM);
    }
    add_bit(parity_odd(byte) ? ONE_PULSENUM : ZERO_PULSENUM);
  }

  ESP_LOGD(TAG, "Sending %s", to_hex(words.data(), words.size()).c_str());

  portENTER_CRITICAL(&this->lock_);
  this->tx_active_ = true;
  this->pulse_count_ = 0;
  this->bit_count_ = 0;
  portEXIT_CRITICAL(&this->lock_);
  this->tx_enable_pin_->digital_write(true);

  rmt_transmit_config_t config{};
  config.loop_count = 0;
  config.flags.eot_level = 0;
  esp_err_t err = rmt_transmit(this->tx_channel_, this->tx_encoder_, this->tx_symbols_,
                               num_symbols * sizeof(rmt_symbol_word_t), &config);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Sending failed: %s", esp_err_to_name(err));
    this->tx_enable_pin_->digital_write(false);
    portENTER_CRITICAL(&this->lock_);
    this->tx_active_ = false;
    portEXIT_CRITICAL(&this->lock_);
  }
}

}  // namespace gdoor
}  // namespace esphome
