#pragma once
#include <functional>
#include <string>
#include <vector>
#include "esp_log.h"
#include "esp_err.h"
namespace wetzelmesh
{

    /**
     * Classe de transporte genérica para comunicação entre nós.
     * O gateway usará instâncias dessa classe para receber e enviar pacotes JSON.
     */
    class MeshTransport
    {
    public:
        using RxHandler = std::function<void(const std::string &)>;

        MeshTransport();
        esp_err_t init();
        void setRxHandler(RxHandler handler);

        // Envia um pacote JSON (para UART, BLE, Wi-Fi, etc.)
        esp_err_t send(const std::string &json);

        // Simulação local de recebimento (para testes)
        void simulateRx(const std::string &json);

    private:
        RxHandler rxHandler_;
        static constexpr const char *TAG = "MeshTransport";
    };

} // namespace wetzelmesh
