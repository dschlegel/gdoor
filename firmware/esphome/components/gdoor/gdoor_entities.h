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

#include <algorithm>

#include "esphome/core/defines.h"
#include "gdoor.h"

#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif
#ifdef USE_BUTTON
#include "esphome/components/button/button.h"
#endif
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif

namespace esphome {
namespace gdoor {

#ifdef USE_BINARY_SENSOR
/**
 * Turns on for auto_off ms when a message matching all configured filters is received,
 * e.g. a specific door bell button.
 */
class GDoorBinarySensor : public binary_sensor::BinarySensor, public Component, public Parented<GDoorComponent> {
 public:
  void add_busdata(const std::string &value) { this->busdata_.push_back(value); }
  void set_action(const std::string &value) { this->action_ = value; }
  void set_source(const std::string &value) { this->source_ = value; }
  void set_destination(const std::string &value) { this->destination_ = value; }
  void set_parameters(const std::string &value) { this->parameters_ = value; }
  void set_auto_off(uint32_t auto_off) { this->auto_off_ = auto_off; }

  void setup() override {
    this->publish_initial_state(false);
    this->parent_->add_on_message_callback([this](const GDoorMessage &message) {
      if (this->matches_(message)) {
        this->publish_state(true);
        this->set_timeout("auto_off", this->auto_off_, [this]() { this->publish_state(false); });
      }
    });
  }

 protected:
  bool matches_(const GDoorMessage &message) const {
    return (this->busdata_.empty() ||
            std::find(this->busdata_.begin(), this->busdata_.end(), message.busdata) != this->busdata_.end()) &&
           (this->action_.empty() || this->action_ == message.action) &&
           (this->source_.empty() || this->source_ == message.source) &&
           (this->destination_.empty() || this->destination_ == message.destination) &&
           (this->parameters_.empty() || this->parameters_ == message.parameters);
  }

  std::vector<std::string> busdata_;  // Matches any of them
  std::string action_;
  std::string source_;
  std::string destination_;
  std::string parameters_;
  uint32_t auto_off_{1000};
};
#endif

#ifdef USE_BUTTON
/// Sends a fixed message to the bus when pressed, e.g. to open the door.
class GDoorButton : public button::Button, public Parented<GDoorComponent> {
 public:
  void set_busdata(const std::string &busdata) { this->busdata_ = busdata; }

 protected:
  void press_action() override { this->parent_->send(this->busdata_); }

  std::string busdata_;
};
#endif

#ifdef USE_TEXT_SENSOR
enum GDoorTextSensorField {
  FIELD_BUSDATA,
  FIELD_ACTION,
  FIELD_TYPE,
  FIELD_SOURCE,
  FIELD_DESTINATION,
  FIELD_PARAMETERS,
};

/// Publishes one field of every received message, e.g. to find out the busdata of a button.
class GDoorTextSensor : public text_sensor::TextSensor, public Component, public Parented<GDoorComponent> {
 public:
  void set_field(GDoorTextSensorField field) { this->field_ = field; }

  void setup() override {
    this->parent_->add_on_message_callback([this](const GDoorMessage &message) {
      switch (this->field_) {
        case FIELD_BUSDATA: this->publish_state(message.busdata); break;
        case FIELD_ACTION: this->publish_state(message.action); break;
        case FIELD_TYPE: this->publish_state(message.type); break;
        case FIELD_SOURCE: this->publish_state(message.source); break;
        case FIELD_DESTINATION: this->publish_state(message.destination); break;
        case FIELD_PARAMETERS: this->publish_state(message.parameters); break;
      }
    });
  }

 protected:
  GDoorTextSensorField field_{FIELD_BUSDATA};
};
#endif

}  // namespace gdoor
}  // namespace esphome
