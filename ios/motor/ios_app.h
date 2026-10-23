// Lo que comparten el lanzador (lanzador.mm) y el mando tactil (controles_tactiles.mm).

#pragma once

#import <Foundation/Foundation.h>

// Los textos de la app, los de Android y los propios de iOS (textos.py, textos.json en la app): el
// de esa clave en el idioma elegido en el lanzador o, sin elegir, en el del telefono; si alli
// falta, en ingles. Con formato, como getString(id, args...): NfsmwTextoF(@"fps_valor", 60).
NSString* NfsmwTexto(NSString* clave);
#define NfsmwTextoF(clave, ...) [NSString stringWithFormat:NfsmwTexto(clave), __VA_ARGS__]

// La vibracion del movil con la del juego (controles_tactiles.mm): medio segundo, para probarla
// desde el lanzador.
void NfsmwVibrarUnRato();
