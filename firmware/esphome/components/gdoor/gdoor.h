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
#pragma once

#include <deque>
#include <string>
#include <vector>

#include <driver/rmt_tx.h>
#include <freertos/FreeRTOS.h>

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/components/output/float_output.h"

namespace esphome {
namespace gdoor {

static const uint16_t MAX_WORDLEN = 25;          // Max. words (bytes) of one bus message, incl. checksum
static const uint16_t MAX_BITS = MAX_WORDLEN * 9;  // 8 data bits + 1 parity bit per word

/**
 * One decoded bus message.
 * All byte fields are upper case hex strings, e.g. "A286FD".
 */
struct GDoorMessage {
  std::string busdata;      // Complete message incl. checksum
  std::string action;       // e.g. BUTTON_RING, DOOR_OPEN
  std::string type;         // Hardware type of the sender, e.g. OUTDOOR, INDOOR
  std::string source;       // Address of the sender
  std::string destination;  // Address of the receiver, 000000 if the message has none
  std::string parameters;   // e.g. which button was pressed
  bool valid{false};        // Parity and checksum are correct
};

class GDoorComponent : public Component {
 public:
  void set_rx_pin(InternalGPIOPin *pin) { this->rx_pin_ = pin; }
  void set_tx_pin(InternalGPIOPin *pin) { this->tx_pin_ = pin; }
  void set_tx_enable_pin(InternalGPIOPin *pin) { this->tx_enable_pin_ = pin; }
  void set_rx_threshold(output::FloatOutput *output, float voltage) {
    this->rx_threshold_output_ = output;
    this->rx_threshold_voltage_ = voltage;
  }

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  /**
   * Queue a message to be sent to the bus.
   * @param busdata hex string without checksum, e.g. "0211..."
   * @return false if busdata is not valid hex or too long
   */
  bool send(const std::string &busdata);

  /// Called for every valid message received from the bus.
  void add_on_message_callback(std::function<void(const GDoorMessage &)> &&callback) {
    this->message_callback_.add(std::move(callback));
  }

 protected:
  static void rx_edge_isr(GDoorComponent *arg);
  static bool tx_done_isr(rmt_channel_handle_t channel, const rmt_tx_done_event_data_t *event, void *arg);
  void finish_bit_();
  void finish_message_();

  bool decode_(const uint16_t *counts, uint16_t len, GDoorMessage &message);
  void start_transmission_(const std::vector<uint8_t> &data);

  InternalGPIOPin *rx_pin_{nullptr};
  InternalGPIOPin *tx_pin_{nullptr};
  InternalGPIOPin *tx_enable_pin_{nullptr};
  ISRInternalGPIOPin tx_enable_isr_pin_;
  output::FloatOutput *rx_threshold_output_{nullptr};
  float rx_threshold_voltage_{0};

  // RX state, shared between ISRs and loop(), protected by lock_
  portMUX_TYPE lock_ = portMUX_INITIALIZER_UNLOCKED;
  volatile int64_t last_edge_us_{0};     // Time of the last received carrier edge
  volatile uint16_t pulse_count_{0};     // Carrier edges of the bit currently received, 0: bus idle
  volatile uint16_t bit_count_{0};       // Bits of the message currently received
  uint16_t counts_[MAX_BITS];            // Carrier edges per bit of the message currently received
  volatile bool message_ready_{false};   // ready_counts_ holds a complete message for loop()
  uint16_t ready_len_{0};
  uint16_t ready_counts_[MAX_BITS];
  volatile uint32_t messages_dropped_{0};  // Messages received while loop() did not fetch the previous one
  volatile bool tx_active_{false};       // Sending, RX ignores our own message

  uint16_t decode_counts_[MAX_BITS];     // Copy of ready_counts_ for decoding outside of the lock

  // TX
  rmt_channel_handle_t tx_channel_{nullptr};
  rmt_encoder_handle_t tx_encoder_{nullptr};
  rmt_symbol_word_t tx_symbols_[MAX_BITS + 1];  // Start bit + data bits, must stay valid while sending
  std::deque<std::vector<uint8_t>> tx_queue_;

  CallbackManager<void(const GDoorMessage &)> message_callback_;
};

class MessageTrigger : public Trigger<const GDoorMessage &> {
 public:
  explicit MessageTrigger(GDoorComponent *parent) {
    parent->add_on_message_callback([this](const GDoorMessage &message) { this->trigger(message); });
  }
};

template<typename... Ts> class SendAction : public Action<Ts...>, public Parented<GDoorComponent> {
 public:
  TEMPLATABLE_VALUE(std::string, busdata)

  void play(const Ts &...x) override { this->parent_->send(this->busdata_.value(x...)); }
};

}  // namespace gdoor
}  // namespace esphome
