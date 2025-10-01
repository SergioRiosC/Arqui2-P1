#ifndef INTERCONNECT_H
#define INTERCONNECT_H

#include <cstddef>
#include <vector>

// Forward declaration para evitar dependencias circulares
class Cache;

// Mensajes que el interconnect soportará
enum class BusMsgType { BusRd, BusRdX, BusUpgr, Flush };

// Interconnect interface (a implementar luego)
// La caché llamará a estos métodos cuando necesite notificar al bus.
// Las implementaciones concretas deben distribuir el mensaje a las otras caches.
struct IInterconnect {
    virtual ~IInterconnect() = default;

    // Broadcast: una cache solicita leer un bloque (compartido).
    // Debe devolver true si alguna otra cache tiene la línea en M/E/S y la suministró (opcional).
    virtual bool broadcast_busrd(int requester_id, size_t block_number) = 0;

    // Broadcast: una cache solicita permiso exclusivo (BusRdX), invalidando otros.
    // Debe invalidar/forzar writeback en otras caches si requieren.
    virtual void broadcast_busrdx(int requester_id, size_t block_number) = 0;

    // Broadcast: una cache requiere invalidar compartidos (BusUpgr).
    virtual void broadcast_busupgr(int requester_id, size_t block_number) = 0;

    // Notificar flush (escribir back) — opcionalmente usado por interconnect para recoger datos.
    virtual void broadcast_flush(int requester_id, size_t block_number, const double *block_data, size_t words) = 0;
};

// Implementación nula: no hace nada. Útil mientras no tengas interconnect global.
struct NullInterconnect : public IInterconnect {
    bool broadcast_busrd(int requester_id, size_t block_number) override { return false; }
    void broadcast_busrdx(int requester_id, size_t block_number) override {}
    void broadcast_busupgr(int requester_id, size_t block_number) override {}
    void broadcast_flush(int requester_id, size_t block_number, const double *block_data, size_t words) override {}
};

#endif // INTERCONNECT_H
