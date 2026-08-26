#pragma once

#include <Arduino.h>

// Decide quando il gateway deve mandare una notifica per conto proprio.
//
// La regola e' una sola: se il gateway e' collegato a Sheltr Cloud, le notifiche
// le manda il portale (conosce gli indirizzi e le preferenze di ogni utente).
// Se il portale non c'e' — non configurato, oppure irraggiungibile — ma c'e'
// internet, le manda il gateway via email agli indirizzi impostati in Sistema.
// Cosi' non arrivano doppioni e non si resta mai scoperti.
namespace notifier {

void begin();
void loop();

// Scatto di un ingresso: lo segnala inputs.cpp.
void inputTriggered(size_t index);

}  // namespace notifier
