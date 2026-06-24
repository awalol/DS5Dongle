//
// Wake-on-LAN sobre WiFi (ENABLE_WOL).
//
// Quan el comandament es connecta (s'obre el canal L2CAP HID Interrupt), es
// puja el WiFi en mode estació, s'envia un "magic packet" a la MAC del PC
// definida a secrets.h i es torna a baixar el WiFi. Tot de manera NO bloquejant
// (màquina d'estats servida des del bucle principal) per no disparar el watchdog
// i per no interferir amb el Bluetooth del comandament.
//

#ifndef DS5_BRIDGE_WOL_H
#define DS5_BRIDGE_WOL_H

// Inicialitza l'estat intern. Cridar un cop, després de cyw43_arch_init().
void wol_init();

// Sol·licita una seqüència de WoL. Segur de cridar des del context del callback
// de Bluetooth: només marca una petició; la feina real (WiFi/UDP) es fa a
// wol_tick(). Té anti-rebot intern per no repetir-se en reconnexions ràpides.
void wol_request();

// Avança la màquina d'estats. Cridar a cada iteració del bucle principal.
void wol_tick();

#endif // DS5_BRIDGE_WOL_H
