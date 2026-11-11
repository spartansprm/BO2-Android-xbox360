# Black Ops II para Android

Port nativo de **Call of Duty: Black Ops II (Xbox 360)** a Android ARM64, con Campaña, Zombies y Multijugador,
controles táctiles y renderizador Vulkan. Versión **0.55.0 (beta)**.

No es un emulador: el código PowerPC del juego se traduce a C++ durante la compilación y se ejecuta de forma nativa.
Por eso **este repositorio no contiene datos del juego y nunca los contendrá**: cada persona compila el programa a
partir de su propia copia, y lo que se genera se queda en su equipo.

> **Esto es una beta.** Funciona y se puede jugar, pero el rendimiento depende mucho del teléfono y hay fallos
> conocidos. Lee las secciones de rendimiento y de problemas antes de empezar.

---

## Lo que necesitas

### Del juego

Tu propia copia de **Black Ops II de Xbox 360** con el **Title Update 18**. La compilación comprueba que sea esa
versión exacta (title id `415608C3`, versión `0.0.18.14`) y se detiene si no coincide, así que otras versiones no
sirven.

De ahí salen cuatro archivos:

| Archivo | Para qué |
|---|---|
| `default_mp.xex` + `default_mp.xexp` | Zombies y Multijugador |
| `default.xex` + `default.xexp` | Campaña |

Si solo quieres Zombies y Multijugador, con los dos primeros basta (`--program mp` al compilar).

Los DLC son opcionales y se leen de los paquetes que aportes tú.

### Del teléfono

- Android 9 o superior, **ARM64**
- Vulkan compatible
- La CPU se compila para ARMv8.2-A con FP16 y dot product

---

## Cómo se añade el juego

Son dos momentos distintos y conviene no confundirlos.

**1. Al compilar (en el PC).** Necesitas los cuatro ejecutables de arriba. Si tu copia está en formato GOD y no sabes
extraerlos, el propio proyecto genera una herramienta para hacerlo desde el móvil:

```bash
./build.sh --stage extractor
```

Instalas `dist/BO2-input-extractor.apk`, eliges la carpeta del juego, preparas Zombies/Multijugador y Campaña, y te
guarda dos ZIP que pasas al PC. Esa herramienta se instala aparte y no incluye código nativo del juego.

**2. Al jugar (en el teléfono).** La primera vez, el lanzador te pide la carpeta del juego: la que contiene
`00007000` y `000B0000`. Se guarda el permiso de lectura y ya no vuelve a preguntar. De ahí salen los mapas, texturas,
sonidos y vídeos. «Cambiar carpeta», en el mismo menú, la olvida.

Los shaders se traducen en el teléfono la primera vez que aparecen, lo que provoca tirones al principio de cada mapa.
Si tienes una biblioteca de shaders ya preparada, puedes importarla desde Ajustes para evitarlos.

---

## Ajustes del lanzador

Se guardan al tocarlos y se aplican la próxima vez que pulsas Jugar.

### Resolución

Cambia a la vez la resolución interna de la escena 3D y la de la imagen final.

| Opción | Escena 3D | Salida | Comentario |
|---|---|---|---|
| **Original (como Xbox 360)** | 880×720 | 1280×720 | Lo mismo que hacía la consola. La más rápida |
| **HD · 720p** | 1280×720 | 1280×720 | El estirado horizontal del juego pasa a tener columnas reales |
| **Full HD · 1080p** | 1920×1080 | 1920×1080 | La más exigente, con diferencia |

El coste sube con los píxeles: HD es ≈1,45× el trabajo de Original y Full HD ≈3,3×. En «Pantalla completa» la imagen
se dibuja todavía más ancha (2340×1080 en un panel 19,5:9), así que suma otro 22 % sobre Full HD.

### Tipo de pantalla

- **Pantalla completa**: ocupa toda la pantalla sin deformar, ensanchando el campo de visión de verdad. En Campaña,
  de momento, se queda en 16:9
- **Original**: 16:9 con barras negras
- **Pantalla completa estirada**: ocupa todo, pero deforma la imagen

### Nitidez (detalle de texturas)

- **Máxima (recomendada)**: texturas completas
- **Media**: un nivel de mip por debajo
- **La del preset del juego**: lo que diga el preset, que por defecto es bajo y se ve borroso aunque subas la
  resolución

### Calidad del escenario

Detalle de los modelos a distancia, distancia de dibujado, sombras, luces dinámicas, efectos y marcas.

- **La del menú del juego** (muy baja por defecto)
- **Baja**
- **Media**
- **Alta (como Xbox 360)**: los valores originales de la consola. Es la opción más cara, sobre todo por las sombras
  de sol y focos, y es la que activa las luces dinámicas (**ojo a los errores gráficos descritos más abajo**)

### Filtrado anisotrópico

Mantiene nítidos suelos y paredes vistos en ángulo: **el del juego (2×–4×)**, **4×** u **8×**.

---

## Ajustes dentro del juego

El menú de gráficos del propio juego permite ajustar cada opción por separado, y manda sobre el preset mientras
juegas (al volver a pulsar Jugar en el lanzador se vuelve a aplicar el del lanzador):

| Opción | Qué cambia |
|---|---|
| TEXTURE DETAIL | Nitidez de las texturas |
| MODEL DETAIL | Con cuánta distancia los modelos pasan a versiones simples |
| DRAW DISTANCE | Hasta dónde se dibuja el mundo |
| FAR OBJECTS | Qué pasa con lo que queda más allá: ocultarlo o dibujarlo simple |
| SHADOWS | Sombras de sol y focos. De lo más caro |
| DYNAMIC LIGHTS | Luces de fogonazos, explosiones y efectos |
| SCREEN EFFECTS | Profundidad de campo, llamas, agua en pantalla y nubes |
| DECALS | Sangre, quemaduras e impactos, y hasta qué distancia se ven |
| CORPSES AND DEBRIS | Cuántos cuerpos y restos permanecen |

---

## Rendimiento

Lo medido hasta ahora, para que sepas qué esperar. Los números son del contador interno del juego, no de la tasa real
de la pantalla, y vienen de partidas concretas: tómalos como orden de magnitud.

**Samsung Galaxy A54** (Exynos 1380, GPU **Mali-G68**), que ha sido el teléfono de referencia:

| Configuración | Aproximado |
|---|---|
| Original 880×720, ajustes al mínimo | ~45 FPS |
| Full HD / Pantalla completa, Calidad del escenario Alta | ~20–25 FPS |

En ese teléfono la GPU es el límite: con calidad alta a 1080p el trabajo gráfico ronda los 30–37 ms por imagen. Además
llega pronto a estado térmico severo, y los núcleos grandes bajan de 2,4 GHz a ~1,6 GHz, así que el rendimiento cae
durante los primeros minutos y luego se estabiliza más abajo.

**POCO X7 Pro** (GPU **Mali-G720**): **60 FPS con todos los ajustes al máximo**, sin necesidad de recortar nada.

La diferencia entre los dos teléfonos da la medida de lo que pide el juego: en una GPU de gama alta va holgado a
tope, y en una de gama media hay que elegir. Si el tuyo es de gama media, lo más rentable es **Original 880×720** con
Nitidez Máxima y Calidad del escenario Baja o Media: subir la resolución cuesta mucho más que subir la calidad del
escenario.

---

## Problemas conocidos

- **Errores gráficos con DYNAMIC LIGHTS activadas.** Al activar las luces dinámicas (en el menú de gráficos del
  juego, o indirectamente al elegir «Calidad del escenario: Alta», que las enciende) aparecen errores gráficos.
  Mientras no esté resuelto, la solución es dejar DYNAMIC LIGHTS en **OFF**, o usar una calidad de escenario por
  debajo de Alta.
- **Tirones al entrar en un mapa nuevo** mientras se traducen y compilan los shaders y las pipelines de Vulkan. Baja
  en partidas siguientes porque se guardan en caché.
- **Calentamiento** en sesiones largas, sobre todo a resoluciones altas.
- La Campaña, de momento, se dibuja en 16:9 aunque elijas Pantalla completa.

---

## Créditos y licencia

Este port parte de **[bo2-360-nx](https://github.com/AlvaroEPena/bo2-360-nx), de Álvaro E. Peña**, que es quien hizo
el trabajo de fondo: la recompilación estática de Black Ops II desde el ejecutable de Xbox 360 y las versiones de
Nintendo Switch y Windows. Sin ese proyecto este no existiría. La adaptación a Android, el renderizador Vulkan para
GPU móviles, los controles táctiles y el lanzador son el añadido de este repositorio. Su desarrollo fue asistido con
IA.

También se apoya en el **ReXGlue SDK** (con partes de **Xenia**) y en **XenosRecomp**, de hedge-dev.

El código de este proyecto se distribuye bajo [GPL-3.0](LICENSE). Las dependencias conservan sus licencias, recogidas
en [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) y [android/THIRD_PARTY_NOTICES.md](android/THIRD_PARTY_NOTICES.md).

Call of Duty y Black Ops II son marcas de Activision. Este proyecto no está afiliado ni respaldado por Activision,
Microsoft o Google, y no incluye datos del juego.
