#include "ramses_esp.h"
#include "esphome/core/log.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <algorithm>
#include <type_traits>
#include "driver/gpio.h"

static const char *const TAG = "ramses_esp";

namespace esphome {
namespace ramses_esp {

// FreeRTOS queues copy items with memcpy.
static_assert(std::is_trivially_copyable<RamsesMessage>::value,
              "RamsesMessage must stay trivially copyable (passed via memcpy through FreeRTOS queues)");

void RamsesESPComponent::setup() {
  ESP_LOGCONFIG(TAG, "Setting up RAMSES ESP component...");

  this->radio_mutex_ = xSemaphoreCreateMutex();
  this->rx_msg_queue_ = xQueueCreate(16, sizeof(RamsesMessage));
  this->tx_msg_queue_ = xQueueCreate(8, sizeof(RamsesMessage));

  if (!this->cc1101_.init(SPI2_HOST, this->sck_pin_, this->mosi_pin_, this->miso_pin_, this->cs_pin_)) {
    ESP_LOGE(TAG, "Failed to initialize CC1101 transceiver!");
    this->mark_failed();
    return;
  }

  if (!this->frame_handler_.init(this->uart_num_, this->gdo0_pin_, this->gdo2_pin_, &this->cc1101_)) {
    ESP_LOGE(TAG, "Failed to initialize RAMSES frame handler!");
    this->mark_failed();
    return;
  }

  this->frame_handler_.set_on_message_callback([this](const RamsesMessage &msg) {
    if (this->rx_msg_queue_ != nullptr) {
      xQueueSend(this->rx_msg_queue_, &msg, 0);
    }
  });

  // TX uses the FIFO; GDO0 reports its threshold and empty states.
  gpio_reset_pin(this->gdo2_pin_);
  gpio_set_direction(this->gdo2_pin_, GPIO_MODE_INPUT);
  gpio_set_pull_mode(this->gdo2_pin_, GPIO_PULLDOWN_ONLY);

  xTaskCreatePinnedToCore(
      RamsesESPComponent::radio_task_trampoline,
      "ramses_radio",
      4096,
      this,
      10,
      &this->radio_task_handle_,
      0
  );

  this->start_tcp_server();
}

void RamsesESPComponent::radio_task_trampoline(void *arg) {
  reinterpret_cast<RamsesESPComponent *>(arg)->radio_task();
}

void RamsesESPComponent::radio_task() {
  ESP_LOGI(TAG, "RAMSES Radio task started");
  while (true) {
    if (!this->paused_) {
      if (xSemaphoreTake(this->radio_mutex_, pdMS_TO_TICKS(10)) == pdTRUE) {
        if (!this->paused_) {
          this->frame_handler_.work();
        }
        xSemaphoreGive(this->radio_mutex_);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

void RamsesESPComponent::pause() {
  if (this->paused_) return;
  ESP_LOGD(TAG, "Pausing RAMSES Radio for external operation...");
  if (xSemaphoreTake(this->radio_mutex_, pdMS_TO_TICKS(200)) == pdTRUE) {
    this->paused_ = true;
    this->frame_handler_.rx_disable();
    this->cc1101_.enter_idle_mode();
  }
}

void RamsesESPComponent::resume() {
  if (!this->paused_) return;
  ESP_LOGD(TAG, "Resuming RAMSES Radio reception...");
  this->cc1101_.apply_ramses_config();
  this->frame_handler_.rx_enable();
  this->paused_ = false;
  xSemaphoreGive(this->radio_mutex_);
}

void RamsesESPComponent::start_tcp_server() {
  this->server_fd_ = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
  if (this->server_fd_ < 0) {
    ESP_LOGE(TAG, "Unable to create TCP socket: errno %d", errno);
    return;
  }

  int opt = 1;
  setsockopt(this->server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  fcntl(this->server_fd_, F_SETFL, O_NONBLOCK);

  struct sockaddr_in dest_addr;
  dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  dest_addr.sin_family = AF_INET;
  dest_addr.sin_port = htons(this->port_);

  int err = bind(this->server_fd_, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
  if (err != 0) {
    ESP_LOGE(TAG, "Socket unable to bind on port %u: errno %d", this->port_, errno);
    close(this->server_fd_);
    this->server_fd_ = -1;
    return;
  }

  err = listen(this->server_fd_, 4);
  if (err != 0) {
    ESP_LOGE(TAG, "Error occurred during listen: errno %d", errno);
    close(this->server_fd_);
    this->server_fd_ = -1;
    return;
  }

  ESP_LOGI(TAG, "RAMSES HGI80 TCP Server listening on port %u", this->port_);
}

void RamsesESPComponent::loop() {
  RamsesMessage rx_msg;
  while (this->rx_msg_queue_ != nullptr && xQueueReceive(this->rx_msg_queue_, &rx_msg, 0) == pdTRUE) {
    std::string hgi80 = rx_msg.to_hgi80();
    this->broadcast_hgi80(hgi80);
    for (auto &cb : this->on_message_callbacks_) {
      cb(hgi80);
    }
  }

  this->handle_tcp_clients();

  if (!this->paused_) {
    this->process_tx_queue();
  }
}

void RamsesESPComponent::handle_tcp_clients() {
  if (this->server_fd_ < 0) return;

  struct sockaddr_in source_addr;
  socklen_t addr_len = sizeof(source_addr);
  int client_fd = accept(this->server_fd_, (struct sockaddr *)&source_addr, &addr_len);
  if (client_fd >= 0) {
    fcntl(client_fd, F_SETFL, O_NONBLOCK);
    this->client_fds_.push_back(client_fd);
    ESP_LOGI(TAG, "TCP Client connected from %s (Total clients: %d)",
             inet_ntoa(source_addr.sin_addr), (int)this->client_fds_.size());
  }

  for (auto it = this->client_fds_.begin(); it != this->client_fds_.end();) {
    int fd = *it;
    char rx_buffer[256];
    int len = recv(fd, rx_buffer, sizeof(rx_buffer) - 1, 0);
    if (len > 0) {
      rx_buffer[len] = '\0';
      std::string line(rx_buffer);
      line.erase(std::remove(line.begin(), line.end(), '\r'), line.end());
      line.erase(std::remove(line.begin(), line.end(), '\n'), line.end());
      if (!line.empty()) {
        ESP_LOGI(TAG, "TCP Rx Command: %s", line.c_str());
        this->send_hgi80_command(line);
      }
      ++it;
    } else if (len == 0 || (len < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
      ESP_LOGI(TAG, "TCP Client disconnected");
      close(fd);
      it = this->client_fds_.erase(it);
    } else {
      ++it;
    }
  }
}

void RamsesESPComponent::broadcast_hgi80(const std::string &hgi80) {
  std::string line = hgi80 + "\r\n";
  for (auto it = this->client_fds_.begin(); it != this->client_fds_.end();) {
    int fd = *it;
    int sent = send(fd, line.c_str(), line.length(), 0);
    if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
      close(fd);
      it = this->client_fds_.erase(it);
    } else {
      ++it;
    }
  }
}

bool RamsesESPComponent::send_hgi80_command(const std::string &cmd) {
  RamsesMessage msg;
  if (!msg.from_hgi80(cmd)) {
    ESP_LOGW(TAG, "Invalid HGI80 command format: %s", cmd.c_str());
    return false;
  }

  if (this->tx_msg_queue_ != nullptr) {
    return xQueueSend(this->tx_msg_queue_, &msg, pdMS_TO_TICKS(100)) == pdTRUE;
  }
  return false;
}

void RamsesESPComponent::process_tx_queue() {
  RamsesMessage tx_msg;
  if (this->tx_msg_queue_ != nullptr && xQueuePeek(this->tx_msg_queue_, &tx_msg, 0) == pdTRUE) {
    constexpr uint32_t MIN_TX_DELAY_MS = 50;
    uint32_t now = millis();
    uint32_t last_frame = std::max(this->frame_handler_.last_frame_ms(), this->last_tx_ms_);
    if (this->frame_handler_.rx_in_progress() || now - last_frame <= MIN_TX_DELAY_MS) return;

    if (xSemaphoreTake(this->radio_mutex_, pdMS_TO_TICKS(200)) == pdTRUE) {
      if (xQueueReceive(this->tx_msg_queue_, &tx_msg, 0) != pdTRUE) {
        xSemaphoreGive(this->radio_mutex_);
        return;
      }

      ESP_LOGI(TAG, "Transmitting RAMSES packet: %s", tx_msg.to_hgi80().c_str());

      this->frame_handler_.rx_disable();
      std::vector<uint8_t> raw_frame = tx_msg.to_raw_frame();

      this->cc1101_.enter_tx_mode();
      size_t sent = 0;
      while (sent < std::min<size_t>(4, raw_frame.size())) {
        this->cc1101_.write_fifo(raw_frame[sent++]);
      }

      uint32_t feed_start = millis();
      while (sent < raw_frame.size() && millis() - feed_start < 300) {
        if (gpio_get_level(this->gdo2_pin_) == 0) {
          size_t block_end = std::min(sent + 5, raw_frame.size());
          while (sent < block_end) this->cc1101_.write_fifo(raw_frame[sent++]);
        }
      }

      if (sent != raw_frame.size()) {
        ESP_LOGW(TAG, "TX FIFO feed timed out (%u/%u bytes)",
                 (unsigned) sent, (unsigned) raw_frame.size());
      } else {
        this->cc1101_.fifo_end();
        uint32_t empty_start = millis();
        while (gpio_get_level(this->gdo2_pin_) == 0 && millis() - empty_start < 100) {
          vTaskDelay(pdMS_TO_TICKS(1));
        }
        if (gpio_get_level(this->gdo2_pin_) == 0) {
          ESP_LOGW(TAG, "TX FIFO-empty signal timed out");
        }
      }
      this->cc1101_.enter_idle_mode();
      this->last_tx_ms_ = millis();

      this->cc1101_.apply_ramses_config();
      this->frame_handler_.rx_enable();

      xSemaphoreGive(this->radio_mutex_);
    }
  }
}

void RamsesESPComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "RAMSES ESP Transceiver & Gateway:");
  ESP_LOGCONFIG(TAG, "  SCK Pin: GPIO%d", this->sck_pin_);
  ESP_LOGCONFIG(TAG, "  MOSI Pin: GPIO%d", this->mosi_pin_);
  ESP_LOGCONFIG(TAG, "  MISO Pin: GPIO%d", this->miso_pin_);
  ESP_LOGCONFIG(TAG, "  CS Pin: GPIO%d", this->cs_pin_);
  ESP_LOGCONFIG(TAG, "  GDO0 Pin (UART RX): GPIO%d", this->gdo0_pin_);
  if (this->gdo2_pin_ != GPIO_NUM_NC) {
    ESP_LOGCONFIG(TAG, "  GDO2 Pin: GPIO%d", this->gdo2_pin_);
  }
  ESP_LOGCONFIG(TAG, "  UART Port: UART%d", this->uart_num_);
  ESP_LOGCONFIG(TAG, "  TCP Server Port: %u", this->port_);
}

} // namespace ramses_esp
} // namespace esphome
