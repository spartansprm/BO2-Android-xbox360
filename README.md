# Black Ops II para Android

Port nativo de **Call of Duty: Black Ops II (2012, Xbox 360)** a Android ARM64, con Campaña, Zombies y Multijugador,
controles táctiles (o mando) y renderizador Vulkan. Versión **0.65.0 (beta)**.

No es un emulador: el código PowerPC del juego se traduce a C++ y se ejecuta de forma nativa. Necesitas tu propia copia
del juego; este repositorio no contiene datos del juego.

> **Beta.** Los tres modos arrancan y se pueden jugar, pero el rendimiento depende mucho del teléfono y hay fallos
> conocidos (más abajo).

---

## Del juego

Tu copia de **Black Ops II de Xbox 360** (formato GOD o `.iso`) con el **Title Update 18** (title id `415608C3`,
versión `0.0.18.14`).

Al abrir la app por primera vez, el lanzador pide la carpeta del juego: la que contiene estas carpetas de tu copia:

| Carpeta | Para qué |
|---|---|
| `00007000` o el archivo `.iso` | El juego (obligatorio): la carpeta del formato GOD, o en su lugar el archivo `.iso` del juego |
| `000B0000` | Title Update 18 (obligatoria, también con la `.iso`) |
| `00000002` | Contenido descargable, mapas DLC (opcional) |

Con la `.iso`, el lanzador comprueba que sea de Black Ops II y avisa si no lo es. Probado con copias en español, inglés
y portugués; solo necesitas la TU18 de tu región. Si tienes problemas con los archivos, háblanos en el
[grupo de Discord](https://discord.gg/hdmde3Snr) te ayudaremos.

---

## Del teléfono

- Android 9 o superior, **ARM64**, con CPU **ARMv8.2** o más nueva (Snapdragon 845, Exynos 990, Helio G90, Dimensity
  o posteriores).
- **Vulkan 1.2**, o **Vulkan 1.1** con las extensiones `VK_EXT_descriptor_indexing`, `VK_EXT_scalar_block_layout`,
  `VK_KHR_uniform_buffer_standard_layout` y `VK_KHR_sampler_mirror_clamp_to_edge`.

En **Ajustes → Biblioteca → Driver de GPU** el lanzador comprueba tu GPU y te dice si es compatible.

---

## Ajustes del lanzador

Se aplican la próxima vez que pulsas Jugar. El lanzador y sus menús están en español, inglés, portugués y ruso (se elige
al abrirlo por primera vez; los textos del juego dependen de tu copia).

**Controles**: distribución táctil, perfiles y editor, con botón **CORRER** (carrera automática) y la opción de correr
también con el joystick. En el editor puedes quitar los controles táctiles; con un mando conectado se ocultan solos.
Durante la partida, desliza desde el borde izquierdo para abrir el menú de controles o salir.

**Imagen**:

| Ajuste | Opciones |
|---|---|
| Resolución | Original 880×720 (como Xbox 360), HD 1280×720 o Full HD 1920×1080. La más alta cuesta ≈3,3× más que Original |
| Tipo de pantalla | Pantalla completa (sin deformar), Original (16:9 con barras) o Estirada |
| Nitidez | Máxima (recomendada), Media o la del preset del juego |
| Calidad del escenario | La del menú del juego, Baja, Media o Alta (como Xbox 360) |
| Filtrado anisotrópico | El del juego (2×–4×), 4× u 8× |
| Memoria para texturas *(experimental)* | 384 MB, 768 MB o 1 GB. Subirla reduce las caídas bruscas de FPS si tu teléfono tiene RAM de sobra |
| Preparar shaders antes de jugar | Desactivado por defecto. Activado, la primera vez en cada mapa (y en el menú) espera unos segundos en una pantalla de progreso, así evitas tirones en celulares de gama media-baja |

**Biblioteca**:

- **Biblioteca de shaders**: importa shaders ya preparados para evitar los tirones iniciales.
- **Driver de GPU** *(solo Adreno / Snapdragon)*: carga un driver gráfico como **Mesa Turnip** o un driver de Qualcomm
  actualizado, en un ZIP como los de los emuladores. Sirve para teléfonos con drivers antiguos o con fallos (pantalla
  negra, partes sin dibujar). Si el driver no funciona, el juego vuelve solo al del
  sistema.
- **Guardar registro de la última partida**: crea un ZIP con el registro del juego y los datos de tu GPU para enviarlo
  si tienes un problema.
- **Partidas guardadas** *(experimental)*: copia tu progreso a la carpeta del juego (`bo2-saves`), para no perderlo al
  desinstalar o actualizar la app.

---

## Rendimiento

En un **Samsung Galaxy A54** (Mali-G68) va a unos **45 FPS** a resolución Original con ajustes bajos, y a unos
**20–25 FPS** en Full HD con calidad Alta; la GPU es el límite y el teléfono se calienta. En un **POCO X7 Pro**
(Mali-G720) va a **60 FPS** con todo al máximo.

En gama media, lo que más ayuda es **Original 880×720** con Nitidez Máxima y Calidad del escenario Baja o Media.

---

## Problemas conocidos

- **Las luces dinámicas están desactivadas** siempre (fogonazos y explosiones sin luz propia): en algunas GPU, como la
  Mali-G720, cerraban el juego.
- Algún tirón la primera vez en cada mapa mientras se compilan las pipelines de Vulkan (se guardan en caché). Los
  shaders ya se traducen en segundos. «Preparar shaders antes de jugar» los reduce a cambio de una espera corta.
- Calentamiento en sesiones largas, sobre todo a resoluciones altas.
- La Campaña, de momento, se dibuja en 16:9 aunque elijas Pantalla completa.

---

## Créditos y licencia

Este port parte de **[bo2-360-nx](https://github.com/AlvaroEPena/bo2-360-nx), de Álvaro E. Peña**, que hizo el trabajo
de fondo: la recompilación estática de Black Ops II desde el ejecutable de Xbox 360 y las versiones de Nintendo Switch
y Windows. La adaptación a Android, el renderizador Vulkan para GPU móviles, los controles táctiles y el lanzador son
el añadido de este repositorio.

El desarrollo de este port fue asistido con IA.

También se apoya en el **ReXGlue SDK** (con partes de **Xenia**), en **XenosRecomp**, de hedge-dev, y en
**libadrenotools**, de Billy Laws, para los drivers de GPU.

El código se distribuye bajo [GPL-3.0](LICENSE). Las dependencias conservan sus licencias, recogidas en
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) y [android/THIRD_PARTY_NOTICES.md](android/THIRD_PARTY_NOTICES.md).

Call of Duty y Black Ops II son marcas de Activision. Este proyecto no está afiliado ni respaldado por Activision,
Microsoft o Google, y no incluye datos del juego.
