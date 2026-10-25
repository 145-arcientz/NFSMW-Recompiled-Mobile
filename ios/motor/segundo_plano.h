// El anfitrion de la LAN en segundo plano (el servicio en primer plano de Android, ServidorLanService).
// iOS congela una app a los pocos segundos de salir de ella y le anula los sockets; con el modo de
// fondo "audio" (Info.plist) no la congela mientras suene algo. Mientras el servidor de este telefono
// esta encendido suena silencio, mezclado con lo que suene en el telefono (no corta la musica).

#pragma once

namespace nfsmw::segundo_plano {

// En el hilo principal. true al arrancar el servidor del anfitrion y false al pararlo. Ademas, al
// volver a la app, si el servidor se cayo (una llamada corta el audio y la app se congela), lo
// vuelve a abrir (nfsmw::servidor::Revivir).
void Mantener(bool si);

}  // namespace nfsmw::segundo_plano
