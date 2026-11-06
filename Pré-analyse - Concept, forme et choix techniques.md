# PianoForge : pré-analyse du concept, de la forme et des choix techniques

*Version 0.1 du 4 novembre 2026. Ce document prolonge les deux analyses du dossier : « Simulation réaliste de piano.md », notée **[R]** (recherche), et « Conception Simulateur Piano Réaliste.docx », notée **[C]** (conception). Les chiffres marqués « calculé ici » sortent des scripts du dossier `studies/`. Les autres viennent des deux documents ou des sources citées en fin de document.*

---

## 0. En bref

- **Le concept.** PianoForge est un piano de concert modélisé puis calibré, pas une banque d'échantillons. Le moteur temps réel ne fait que de la synthèse légère, avec des bancs de résonateurs. Toute la physique coûteuse est calculée en amont. C'est l'architecture du brevet Pianoteq (US7915515B2) et du synthétiseur modal de Bank, et les deux documents convergent sur ce point.
- **La forme.** Un seul cœur DSP alimente plusieurs coquilles. On commence par une **application autonome** : MIDI, audio, enregistrement et réglages. Le **plugin** (CLAP, VST3, AU) sort ensuite du même code à moindre coût. Un **boîtier embarqué** reste possible plus tard, à condition de respecter dès maintenant quelques contraintes : peu de RAM, aucun GPU, du SIMD portable.
- **Les langages.** Le moteur temps réel est en **C++20**, sans framework. L'atelier de calibration, la « forge », est en **Python**. L'application et le plugin passent par **JUCE 8**. Rust n'est une alternative crédible pour le moteur que si tu le maîtrises nettement mieux que le C++.
- **La « phase de génération longue ».** Elle existe, mais elle ne consiste pas à pré-calculer des échantillons. Elle se décompose en trois temps. La **calibration** prend des heures, une fois par instrument. La **cuisson** prend de quelques millisecondes à quelques secondes, à chaque changement de réglage. Le **rendu haute qualité hors ligne** de tes prises peut être plus lent que le temps réel.
- **La faisabilité.** Le pire cas, pédale enfoncée avec tout le clavier qui résonne, représente 16 000 à 22 000 résonateurs, soit 6 à 8 GFLOP/s (calculé ici). Un seul cœur de processeur récent suffit. Le vrai goulot d'étranglement est la **donnée de calibration**, pas le calcul.
- **Une référence haute fidélité existe déjà sur ta machine.** Kontakt 8 et la bibliothèque Noire (un Yamaha CFX) y sont installés. Un **banc de référence** fera jouer Kontakt automatiquement selon un protocole de mesure et enregistrera chaque rendu. Le même protocole, rendu ensuite par PianoForge, donnera des paires directement comparables. C'est **le premier outil à construire** (§6.2).
- **Deux pièges** n'apparaissent dans aucun des deux documents. Le premier est numérique : en float32, la forme « directe » des résonateurs désaccorde le grave. L'erreur atteint −1,6 cent sur A0 (calculé ici), assez pour fausser les battements d'unisson. Le second concerne la latence : elle se joue autant sur le pilote audio (surtout sous Windows) que sur le moteur.

---

## 1. Revue critique des deux documents

### 1.1 Ce qui est acquis

Les deux documents s'accordent sur l'essentiel, qu'on peut considérer comme le socle du projet :

1. Le piano est un **système couplé** : marteau non linéaire, cordes raides désaccordées, chevalet, table d'harmonie, résonance sympathique. Sommer 88 sons indépendants ne suffit pas.
2. La **synthèse modale hybride** est la bonne voie. Chaque partiel est un résonateur amorti, ses paramètres sont calculés ou mesurés hors ligne, et un marteau joint à une attaque mesurée l'excite en temps réel.
3. Les **différences finies** servent de référence de vérité hors ligne, pas de moteur temps réel.
4. Le **temps réel est strict** : aucune allocation, aucun verrou, des files sans verrou, des tampons de 64 à 128 échantillons.
5. Le **caractère d'une marque** s'obtient par calibration sur des enregistrements, pas par une formule.

### 1.2 Fiabilité respective

**[R] fait référence.** Il cite des sources primaires (articles JASA, DAFx, IEEE, brevets), signale explicitement les valeurs non vérifiées, et ses dépôts open source ont été vérifiés par clonage.

**[C] est un complément**, à utiliser avec prudence. Il mélange des sources primaires et des sources faibles : Scribd, ResearchGate, forums, Reddit, et même un fil de forum intitulé « Conversation with AI ». Ses paramètres numériques par marque ne sont pas sourcés. Son apport utile tient aux équations du modèle de référence hors ligne : manche de marteau en poutre de Timoshenko, contact de Hunt-Crossley avec amortissement, Euler-Bernoulli avec pertes, couplage longitudinal. *Remarque : ses 68 équations sont des images et n'ont pas pu être relues ici ; elles ne sont pas nécessaires à ce stade.*

### 1.3 Points à corriger dans [C]

| Passage de [C] | Problème | Ce qu'on retient |
|---|---|---|
| §6, « Moteur hybride recommandé » : les cordes en guides d'onde | Le passage contredit le §2 de [C] et [R], qui recommandent le modal. Dans l'aigu, les lignes à retard deviennent trop courtes. | Cordes **modales**. Un guide d'onde reste envisageable pour quelques notes graves. |
| §4 : « table d'harmonie en érable dur » du Steinway | C'est une erreur : la table est en épicéa (le tableau de [C] le dit lui-même), et c'est la ceinture qui est en érable. | — |
| §4 : valeurs de p, de τ et un Q élevé « entre 4 et 9 kHz » par marque | Aucune source. [R] montre qu'aucune mesure comparative de ces pianos n'existe. | Ce sont au mieux des hypothèses, que la calibration remplacera. |
| §4, tableau : feutre Steinway « sans durcisseur » et « imprégné chimiquement » | Les deux affirmations se contredisent, et [R] attribue l'absence de durcisseurs au Kawai. | À ignorer. |
| §6 : « 64 à 128 échantillons, soit 1,3 à 2,7 ms de latence » | C'est la durée d'un seul tampon, pas la latence de la touche au son. | Budget complet au §4.6. |
| §6 : convolution « UPOLS » sans latence | La convolution à partitionnement uniforme ajoute un bloc de latence. La latence nulle demande un partitionnement non uniforme (Gardner). | FFTConvolver en mode TwoStage. |
| §6 : le suréchantillonnage du marteau « garantit une stabilité inconditionnelle » | Le suréchantillonnage réduit l'erreur mais ne garantit rien. Ce sont les schémas qui conservent l'énergie qui garantissent la stabilité ([R] : Van Walstijn, DAFx-24). | Suréchantillonnage ×2 à ×4 et schéma stable. |
| Extrait C++ (guide d'onde) | Le retard est entier, sans retard fractionnaire : jusqu'à 0,5 échantillon d'erreur, soit environ 75 cents sur C8. Le retard de groupe du passe-tout n'est pas compensé. Le passe-tout unique d'ordre 1 ne suit pas la formule de Rauhala-Välimäki. Un modulo est calculé à chaque échantillon. | Code illustratif, à ne pas reprendre. |
| Extrait Faust | La compensation `n_sections * 1.5` est arbitraire. `piano_dispersion_filter` renvoie aussi le retard à compenser (deux sorties, à vérifier dans la documentation Faust), donc l'extrait ne compile probablement pas tel quel. | Repartir des exemples Faust officiels (`piano.dsp`). |
| Tableau open source : `CWBudde/algo-piano`, `fan455` | Ces dépôts n'apparaissent pas dans [R] et n'ont pas été vérifiés. | Pistes à vérifier. |

### 1.4 Ce qu'aucun des deux documents ne traite

- **L'application elle-même** : l'enregistrement, les prises, les réglages, la calibration du clavier.
- Le **découpage du code** et les frontières entre modules.
- Le **format des profils d'instrument**, qui sert de contrat entre la forge et le moteur.
- La **précision numérique** des résonateurs.
- Le **budget de latence de bout en bout** et la gigue.
- Le **déterminisme**, qui conditionne le re-rendu et les tests de non-régression.
- La **faisabilité pour un développeur seul**.

C'est l'objet de la suite.

---

## 2. Le concept

### 2.1 Vision

> Un piano de concert modélisé, qu'on joue au clavier MIDI comme un vrai, qui garde chaque prise, et qu'on règle comme le ferait un technicien.

### 2.2 Scénarios d'usage

1. **Jouer.** On branche le clavier et on joue au casque ou sur enceintes, avec une latence imperceptible : 10 ms au plus, avec une gigue d'au plus 1 ms.
2. **Ne jamais perdre une prise.** La capture MIDI tourne en permanence en tâche de fond. Après coup, on peut garder par exemple les dix dernières minutes jouées.
3. **Réécouter et re-rendre.** On rejoue une prise avec un autre instrument, un autre réglage ou une autre position de micros, puis on l'exporte en WAV ou FLAC à qualité maximale.
4. **Régler l'instrument.** On agit sur l'harmonisation (dureté des marteaux), l'accord (tempérament, étirement, largeur d'unisson), les étouffoirs, les résonances, le couvercle, les micros et la salle.
5. **Adapter le clavier.** On règle la courbe de vélocité, la plage de demi-pédale et la vélocité de relâchement.
6. **Comparer.** On passe instantanément de A à B entre deux réglages, sur la même prise. C'est possible parce que le rendu est déterministe.

### 2.3 Principes directeurs

1. **Le temps réel ne calcule que ce qui dépend du jeu.** Tout ce qui dépend seulement de l'instrument et de ses réglages est calculé à l'avance.
2. **Pas de voix, mais 88 notes et environ 230 cordes, toujours présentes.** L'état est conservé par corde. Une note rejouée frappe une corde qui vibre encore : la refrappe est naturelle, et il n'y a jamais de vol de voix. La charge CPU se règle en éteignant les modes inaudibles.
3. **Le rendu est déterministe.** Même prise, même profil et même version du moteur donnent le même audio, bit à bit sur une même plateforme. C'est ce qui rend possibles le re-rendu, l'A/B et les tests de non-régression. Concrètement : graines fixes pour les bruits, ordre de sommation fixe, aucune dépendance à l'horloge murale.
4. **Le moteur ignore l'interface et le système.** C'est une bibliothèque pure, sans framework, exposée par une API C. L'audio, le MIDI, les fichiers et l'interface vivent dans les coquilles.
5. **La donnée est un produit versionné.** Chaque profil d'instrument a une version de schéma et indique la provenance et la licence de ses enregistrements sources.
6. **Les priorités perceptives commandent**, dans l'ordre établi par [R] : l'attaque et le knock, puis l'inharmonicité et l'accord, la double décroissance et les battements, la sympathie et la pédale, les partiels fantômes jusqu'à A3, et en dernier l'hystérésis du marteau.
7. **Le cœur n'utilise que des licences permissives.** Aucun code GPL ou AGPL n'entre dans le moteur, pour garder toutes les options de diffusion.

### 2.4 Périmètre

| | Contenu |
|---|---|
| **V1, indispensable** | Un profil calibré (« concert 274 cm »). Application autonome sous Windows, macOS et Linux. MIDI USB, CC64 continu, CC66, CC67, vélocité de relâchement. Capture permanente, prises, export MIDI, WAV et FLAC, re-rendu. Réglages du joueur. Mesure de latence intégrée. |
| **V2** | Plugin CLAP, VST3 et AU. Réglages de technicien complets. Second profil. CC88 et MIDI 2.0. Édition simple des prises (vélocités, pédale). Rendu haute qualité. |
| **Plus tard** | Boîtier embarqué (Raspberry Pi 5 ou mini-PC). Profils de marques, selon l'accès aux instruments. Petits réseaux neuronaux pour les transitoires. Différences finies pour les graves en rendu haute qualité. |
| **Hors périmètre** | Pianos droits et historiques au départ. Séquenceur ou DAW complet. Notation. Pédagogie. |

---

## 3. La forme

### 3.1 Options comparées

| Forme | Latence atteignable | Enregistrement et réglages | Effort | Verdict |
|---|---|---|---|---|
| **Application autonome de bureau** | ≤ 10 ms avec ASIO, CoreAudio ou PipeWire | Complets, entièrement maîtrisés | Moyen | **Cible principale** |
| **Plugin dans un DAW** | Celle du DAW | Délégués au DAW | Faible si le cœur est séparé | **Cible secondaire, même cœur** |
| Boîtier dédié (Pi 5, mini-PC) | 3 à 5 ms constatés pour Pianoteq sur Pi ([R]) | Via une interface web | Élevé (système temps réel, intégration) | Plus tard ; les contraintes sont à respecter dès maintenant |
| Web (WASM, Web Audio, Web MIDI) | Médiocre et variable ; pas de Web MIDI dans Safari | Limités | Moyen | Démonstration seulement |
| Mobile (iOS AUv3, Android) | Correcte sur iOS, aléatoire sur Android | — | Élevé | Hors périmètre |

### 3.2 Un cœur, plusieurs coquilles

```
                 ┌──────────────────────────── forge/ (Python) ────────────────────────────┐
enregistrements ►│ analyse (f0, B, décroissances) → ajustement par gradient → profil       │
                 └────────────────────────────────────┬────────────────────────────────────┘
                                                      │ profil d'instrument (physique + assets)
                                                      ▼
┌────────────────────────── pianoforge-core (C++20, sans framework, API C) ──────────────────┐
│   cuisson (thread de fond)  ──►  tables modales  ──►  moteur temps réel : process(bloc)    │
└────────▲────────────────────────▲─────────────────────────▲──────────────────────▲─────────┘
         │                        │                         │                      │
   application autonome        plugin                 pianoforge-cli         pianoforge-py
   (JUCE : audio, MIDI,     (JUCE, ou CLAP +       (rendu hors ligne,     (bindings nanobind :
    prises, interface)       clap-wrapper)          bancs, tests)          la forge pilote
                                                                           le vrai moteur)
```

Le point clé tient dans les bindings Python : **la forge calibre le moteur réel**, pas une réimplémentation. Il ne peut donc pas y avoir d'écart entre le modèle qu'on calibre et celui qu'on joue.

### 3.3 Les fonctions de l'application

**Enregistrement**

- La capture MIDI est permanente, dans un tampon circulaire. Chaque événement porte un horodatage à la microseconde et une vélocité flottante (d'origine 7, 14 ou 16 bits), et les CC continus sont conservés. Une heure de jeu dense représente quelques mégaoctets.
- Une **prise** regroupe les événements, un instantané complet des réglages, l'identifiant et la version du profil, et la version du moteur. Elle est stockée dans un format interne fidèle. On l'exporte en SMF (la haute résolution passe par le CC88) et, plus tard, en MIDI 2.0 Clip File.
- L'audio peut être enregistré directement en sortie, en WAV ou FLAC 24 bits. Par défaut, il est plutôt **re-rendu** à partir de la prise, ce que permet le déterminisme, en qualité temps réel ou haute qualité.
- Les prises forment ensuite le **corpus de tests** du moteur : tout ce que tu joues sert à détecter les régressions.

**Réglages, sur trois niveaux**

- **Joueur** : instrument, volume, courbe de vélocité, diapason, tempérament, micros prédéfinis, réverbération, couvercle, bruits mécaniques.
- **Technicien** : harmonisation par registre et par vélocité, étirement, largeur d'unisson, étouffoirs, demi-pédale, sympathie, duplex et aliquotes, impédance de table. Ces réglages modifient le profil et déclenchent une cuisson.
- **Labo** : CPU par étage, latence mesurée, décrochages audio (xruns), nombre de modes actifs, journal MIDI.
- Les presets sont versionnés et comparables en A/B.

**Calibration du clavier.** Un assistant fait jouer du ppp au fff pour mesurer la plage réelle de vélocité et en déduire la courbe. Il mesure aussi la course de la pédale et son point de demi-pédale. Il détecte enfin ce que le clavier émet : CC88 ou MIDI 2.0, vélocité de relâchement, CC64 continu ou binaire.

**Mesure de latence intégrée.** On place un micro près du clavier. Il capte à la fois le choc mécanique de la touche en fond de course et le son produit par le casque ou les enceintes. L'écart entre les deux est la latence réelle de la touche au son, sans matériel spécial.

---

## 4. L'architecture technique

### 4.1 Les quatre temps du calcul, ou la réponse à la « phase longue »

| Temps | Quand | Durée | Langage | Produit |
|---|---|---|---|---|
| **Calibration (forge)** | Une fois par instrument, puis à chaque amélioration du modèle | Heures | Python et PyTorch, qui pilotent le moteur C++ | Profil physique et assets mesurés |
| **Cuisson** | Au chargement et à chaque réglage de technicien | De quelques ms à quelques s, en tâche de fond | C++ (cœur) | Tables modales prêtes à jouer, mises en cache |
| **Temps réel** | À chaque bloc audio (1,33 ms pour 64 échantillons à 48 kHz) | Quelques centaines de µs | C++ SIMD | Audio |
| **Rendu haute qualité** | À la demande, sur une prise | Peut être plus lent que le temps réel | C++, même moteur avec un budget plus large | WAV ou FLAC |

**Ce que fait la cuisson, concrètement :**

- Elle calcule les fréquences des partiels de chaque corde à partir de f0, de B et des désaccords, et en déduit l'accord étiré.
- Pour chaque note et chaque rang de partiel, elle résout un petit problème aux valeurs propres. Il porte sur les 1 à 3 cordes de l'unisson et leurs 2 polarisations, couplées par l'admittance du chevalet, soit des matrices de 6×6 au plus. Elle en tire les fréquences, les amortissements et les formes des modes couplés. **La double décroissance et les battements deviennent alors gratuits en temps réel**, puisqu'ils sortent de résonateurs indépendants.
- Elle convertit les coefficients pour la fréquence du périphérique (44,1, 48 ou 96 kHz). C'est un avantage net du modal sur l'échantillonnage : aucun rééchantillonnage n'est nécessaire.
- Elle prépare les tables d'excitation par vélocité, les coefficients de la table d'harmonie et les réponses des micros.

Ordre de grandeur : 88 notes × environ 50 rangs donnent quelques milliers de petites décompositions, soit des millisecondes à des dizaines de millisecondes. Quand on tourne un bouton de technicien, la cuisson d'une note est quasi instantanée.

### 4.2 Pourquoi ne pas pré-rendre une banque d'échantillons par simulation lourde

L'idée est tentante : faire tourner un modèle par différences finies pendant des jours, produire 88 notes × N vélocités, puis les jouer comme un échantillonneur. Elle cumule pourtant les défauts des deux mondes. D'un côté, on perd ce que la modélisation apporte : la refrappe sur une corde qui vibre, la demi-pédale continue, la sympathie et les réglages continus. De l'autre, on reprend les défauts de l'échantillonnage : les paliers de vélocité et les gigaoctets de données. Et on n'y gagne pas en réalisme, puisque la qualité est limitée par les paramètres, pas par le solveur (conclusion de [R]).

La simulation lourde garde pourtant deux rôles utiles. Elle sert de **vérité de référence**, pour vérifier qu'une simplification est inaudible. Elle sert aussi de **générateur de données** pour la non-linéarité du grave joué fort (partiels fantômes, précurseur), éventuellement pour entraîner un petit réseau causal.

### 4.3 Le moteur temps réel

On reprend les huit étages de [R], avec les précisions suivantes :

- **Les résonateurs** sont complexes (z ← p·z) et calculés en float32 (voir §4.5). Ils sont rangés en structure de tableaux et traités par paquets de 8 (AVX2) ou de 16 (AVX-512), ou de 4 (NEON). Chaque paquet parcourt tout le bloc audio, avec son état gardé dans les registres. La sortie s'accumule dans un tampon vectoriel par échantillon, et une seule réduction est faite à la fin.
- **La modulation est continue et sans clic.** En forme complexe, on peut modifier le pôle p d'un mode pendant qu'il vibre. Les étouffoirs et les réglages agissent donc directement sur des modes en cours.
- **Le marteau** est calculé échantillon par échantillon pendant le contact (4 ms au plus), suréchantillonné ×2 à ×4, et seulement pour la note frappée. La force vaut F = K·x^p, où x est l'écart entre la position du marteau et le déplacement de la corde au point de frappe (somme pondérée des modes). Comme le marteau frappe l'état modal courant, **la refrappe d'une corde qui vibre est physique**.
- **Un résidu d'attaque mesuré**, qui comprend le knock, se superpose au marteau. C'est la partie la plus critique à l'écoute.
- **Les étouffoirs** sont pilotés par une valeur continue de 0 à 1 par note. Elle se traduit par un amortissement supplémentaire qui dépend de la fréquence, plus une courte excitation de bruit filtré au contact. *Attention au brevet Yamaha JP7476501B2, qui utilise le bruit d'étouffoir comme excitation (voir §7).*
- **La sympathie** passe par un bus de chevalet par régions, d'abord la matrice 8×8 de Bank. Ce bus est injecté dans tous les modes non étouffés.
- **Les modes inaudibles sont éteints.** Un mode étouffé et non excité dont l'énergie passe sous un seuil (par exemple −100 dB) n'est plus calculé. Le pire cas, pédale enfoncée et tout actif, reste budgété.
- **La table d'harmonie** utilise des filtres parallèles du second ordre, sur 4 à 8 régions. **Les micros** passent par une convolution partitionnée non uniforme, sans latence. **La réverbération** de salle (FDN ou convolution) forme un étage séparé.
- **Les bruits mécaniques** (fond de touche, étouffoirs, pédale) sont de courts échantillons déclenchés.

### 4.4 Budget de calcul (calculé ici, `studies/resonator_budget.py`)

Les hypothèses sont grossières : B vaut environ 2×10⁻⁴ en A0, 10⁻⁴ vers C3 et 10⁻² en C8 ; il y a 1, 2 ou 3 cordes par note, et 2 polarisations par corde ; chaque résonateur coûte environ 8 opérations par échantillon à 48 kHz.

| Bande modélisée | Résonateurs (pire cas) | Coût | État en mémoire | Partiels A0 / C4 / C8 |
|---|---|---|---|---|
| ≤ 10 kHz | ≈ 15 800 | ≈ 6,1 GFLOP/s | ≈ 0,3 Mo | 152 / 33 / 2 |
| ≤ 16 kHz | ≈ 22 000 | ≈ 8,4 GFLOP/s | ≈ 0,45 Mo | 196 / 48 / 3 |

En comparaison, Bank et ses collègues tenaient en 2015 environ 17 300 résonateurs en temps réel sur un seul cœur i5 ([R]). Un cœur AVX2 de 2026 dépasse 100 GFLOP/s en crête en float32. Même avec une efficacité de 10 à 20 %, le pire cas tient donc sur un cœur, et l'état tient dans le cache L2. Le coût de la table, des micros et de la réverbération reste à mesurer : c'est l'objet du spike C (§8).

**Mémoire totale.** Les tables modales pèsent quelques Mo. Les résidus d'attaque représentent 88 notes × 12 vélocités × 150 ms × 48 kHz en stéréo float32, soit environ 60 Mo (moitié moins en 16 bits). Le total reste sous 200 Mo, sans streaming disque, ce qui est compatible avec un boîtier embarqué.

### 4.5 Précision numérique : un piège dès la première ligne (calculé ici, `studies/resonator_precision.py`)

Le résonateur classique à deux pôles en forme directe s'écrit y[n] = a1·y[n−1] − a2·y[n−2], avec a1 = 2r·cos θ. Dans le grave, θ est minuscule et a1 vaut presque exactement 2. L'arrondi de a1 en float32 déplace alors la fréquence.

| Note | Forme directe en float32 | Forme complexe en float32 |
|---|---|---|
| A0, 27,5 Hz | **−1,6 cent (−0,025 Hz)** | < 0,0001 cent |
| A1, 55 Hz | −0,6 cent (−0,019 Hz) | < 0,0001 cent |
| A2, 110 Hz | +0,16 cent | < 0,0001 cent |
| C4 | +0,05 cent | < 0,0001 cent |
| ≥ 1 kHz | Négligeable | Négligeable |

Sur 5 s simulées d'un A0, l'écart au signal exact atteint −10 dB sous la crête en forme directe, contre −69 dB en forme complexe.

**Pourquoi c'est grave.** Une erreur de 0,02 à 0,03 Hz est du même ordre que les désaccords d'unisson qui créent les battements et la double décroissance : quelques dixièmes de hertz, avec un verrouillage des fréquences sous 0,3 Hz selon Weinreich. Comme cette erreur varie d'un mode à l'autre, elle brouille les battements du grave.

**Décision.** On utilise des résonateurs complexes (ou en forme couplée) en float32. Ils coûtent deux fois plus d'opérations que la forme directe, mais ils apportent à la fois la précision et la modulation continue. On active aussi FTZ/DAZ dans le thread audio, contre les nombres dénormalisés en fin de décroissance.

### 4.6 Budget de latence de bout en bout

Le tableau suppose 48 kHz et un tampon de 64 échantillons.

| Étape | Valeur typique | Maîtrisable ? |
|---|---|---|
| Scrutation du clavier et firmware | 1 à 5 ms | Non, cela dépend du clavier |
| Transport USB-MIDI | ≤ 1 ms | En MIDI DIN, il faut compter environ 0,64 ms par note : un accord de 10 notes s'étale sur 6 ms environ. **L'USB est préférable.** |
| Pile MIDI du système | < 0,5 ms | — |
| Attente du bloc suivant | 0 à 1,33 ms (mode direct) ou 1,33 ms fixe (mode horodaté) | Oui |
| Tampons de sortie | 1,33 à 2,67 ms | Oui, par la taille du tampon |
| Pilote et convertisseur numérique-analogique | 0,5 à 2 ms | En partie, par le choix de l'interface et du pilote |
| **Total** | **≈ 4 à 11 ms** | |

**Deux modes de traitement des événements.** Le mode **horodaté** place chaque événement à l'échantillon près : la gigue est quasi nulle, au prix d'un bloc de latence en plus. Le mode **direct** applique les événements en début de bloc : la gigue peut atteindre un bloc. À 64 échantillons, cela fait 1,33 ms, au-dessus de la cible de 1 ms, d'où le mode horodaté par défaut. La difficulté est de ramener l'horloge MIDI et l'horloge audio à une même base de temps (QPC sous Windows, mach time sous macOS, CLOCK_MONOTONIC sous Linux), puis de suivre leur dérive.

**Repère physique.** Sur un vrai piano, l'oreille est à environ 1 m de la table d'harmonie, soit environ 3 ms de propagation dans l'air. Des enceintes à 1,5 m ajoutent environ 4,4 ms ; le casque n'ajoute rien.

**Sous Windows, tout se joue sur le pilote.** Deux options conviennent : ASIO, avec le pilote de l'interface audio, ou WASAPI en mode exclusif (ou IAudioClient3 en mode partagé à faible latence). Le mode partagé classique de WASAPI dépasse souvent 10 ms et est à éviter. Les pianos numériques qui font aussi interface audio USB n'ont pas toujours de pilote ASIO, d'où l'intérêt de WASAPI en mode exclusif. Depuis octobre 2025, le SDK ASIO est disponible sous double licence, propriétaire ou GPLv3. Windows MIDI Services, déployé en 2026, apporte enfin le **MIDI multi-client** : jusque-là, un port MIDI ouvert par un DAW était inaccessible aux autres applications.

### 4.7 Threads et temps réel strict

- **Thread audio (temps réel)** : le moteur et rien d'autre. Aucune allocation, aucun verrou, aucune entrée-sortie. FTZ/DAZ activés.
- **Thread MIDI** : horodate les événements et les pousse dans des files SPSC sans attente, vers l'audio et vers l'enregistreur.
- **Thread de cuisson**, en basse priorité : recalcule les tables et les publie par échange atomique de pointeur. L'ancienne table est libérée plus tard, hors du thread audio.
- **Thread d'enregistrement** : vide les files vers le disque.
- **Thread d'interface** : lit les compteurs et les vumètres par des variables atomiques.

**Pas de multicœur dans le callback audio au départ**, puisque le pire cas tient sur un cœur. Plus tard, on pourra répartir les cordes par région sur des threads de travail synchronisés par barrière, en gardant un ordre de sommation fixe pour préserver le déterminisme.

**Outils.** RTSan (Clang 20, sous Linux et macOS seulement, pas Windows) tourne en intégration continue pour détecter les allocations et les verrous dans les fonctions marquées `[[clang::nonblocking]]`. Tracy sert au profilage. Des tests de charge automatisés jouent par exemple un glissando pédale enfoncée.

### 4.8 Le profil d'instrument : le contrat entre la forge et le moteur

```
profiles/concert-274/
  manifest.toml       identifiant, version, version de schéma, provenance et licence des sources
  physics.toml        paramètres physiques par note (lisibles, éditables, comparables en diff)
  presets/*.toml      presets de technicien, exprimés en écarts par rapport à physics.toml
  assets/             résidus d'attaque (FLAC), réponses des micros, bruits
  cache/<hash>.bin    tables cuites, régénérables et jamais versionnées
```

Le profil sépare deux couches. La couche **physique** est petite, éditable et versionnée. La couche **cuite** est un cache régénérable, dont la clé est le hash des entrées et de la version du moteur. Le thread audio ne lit jamais de TOML : le chargement et la cuisson se font en tâche de fond, puis les tables sont échangées de façon atomique.

### 4.9 Déterminisme et tests

- **Le déterminisme** est bit à bit sur une même build et une même plateforme. Entre plateformes, il ne vaut qu'à une tolérance près, car FMA, SIMD et compilateurs diffèrent.
- **Les tests unitaires** couvrent le résonateur, le marteau et les étouffoirs.
- **Les tests de référence** (« golden ») re-rendent des prises connues et comparent le résultat à une empreinte.
- **Les métriques objectives** suivent B par note, T60 par partiel, le centroïde spectral en fonction de la vélocité et les taux de battement.
- **Les bancs de performance** mesurent le temps par bloc et le pire cas.
- **Les tests d'écoute** (ABX, MUSHRA) valident les jalons.
- **Les tests miroir** vérifient qu'à paramètres égaux, le modèle différentiable PyTorch et le moteur C++ donnent le même rendu, à une tolérance près.

---

## 5. Le choix des langages

### 5.1 Les critères, tirés de ta demande

| Critère | Ce qu'il impose |
|---|---|
| Faible latence et faible gigue | Ni ramasse-miettes ni JIT dans le thread audio ; contrôle fin de la mémoire |
| Multiplateforme | Windows (ASIO, WASAPI), macOS (CoreAudio), Linux (ALSA, JACK, PipeWire), et ARM (NEON) pour l'embarqué |
| Optimisé | SIMD exploitable (AVX2, AVX-512, NEON), avec choix du jeu d'instructions à l'exécution |
| Fidèle | Un écosystème scientifique pour la calibration : analyse spectrale, optimisation, différentiation automatique |
| Configurable | Interface riche, sérialisation, presets |
| Phase de génération longue | Calcul hors ligne massif, éventuellement sur GPU |
| Formats plugin (implicite) | SDK CLAP, VST3 et AU |

### 5.2 Matrice des candidats

| Langage | Moteur temps réel | Application et plugin | Forge (calibration) | Commentaire |
|---|---|---|---|---|
| **C++20/23** | Excellent | Excellent : JUCE, CLAP, VST3, AU en natif | Faible | La langue de l'audio. Faust génère du C++, et RTNeural, FFTConvolver, Highway, xsimd et tous les SDK sont en C++. |
| **Rust** | Excellent | Moyen : cpal, midir et egui conviennent ; côté plugins, nih-plug est en maintenance (avec un fork communautaire), clack est bas niveau, et AU reste difficile | Faible, mais PyO3 est excellent pour les bindings | Vraie sûreté mémoire. Le SIMD portable de la bibliothèque standard reste instable, à ma connaissance ; il faut passer par des crates tierces. |
| Zig, Odin | Bon | Faible | — | Prometteurs, mais l'écosystème audio est trop mince. |
| Faust (DSL) | Bon, composant par composant | Par génération de C++ | — | Excellent pour prototyper un filtre ; pas conçu pour gérer 20 000 résonateurs à état dynamique. |
| Go, C#, Java | **Inadapté** (ramasse-miettes) | Bon | Moyen | Les pauses du ramasse-miettes sont incompatibles avec 1 ms de gigue. |
| JS/TS et WASM | Inadapté en natif, médiocre dans le navigateur | Bon pour une interface web | — | Envisageable pour l'interface, pas pour le moteur. |
| **Python** | Inadapté | — | **Excellent** : NumPy, SciPy, PyTorch | Le langage de la forge. |
| Julia | Inadapté | — | Bon | Alternative crédible pour la forge, avec un écosystème d'apprentissage automatique plus restreint. |

### 5.3 Recommandation par couche

| Couche | Choix | Alternative | Raison |
|---|---|---|---|
| Moteur (`pianoforge-core`) | **C++20**, CMake, sans framework, API C | Rust | Écosystème, SIMD, réutilisation de Faust, RTNeural et FFTConvolver |
| SIMD | **Google Highway** (choix SSE4, AVX2, AVX-512 ou NEON à l'exécution) | xsimd ; `std::simd` de C++26 plus tard | Un seul binaire qui exploite la machine où il tourne |
| Convolution | **FFTConvolver** en mode TwoStage (MIT) | WDL | Latence nulle |
| Application et plugin | **JUCE 8** (CLAP via clap-juce-extensions) | CLAP, clap-wrapper, RtAudio, RtMidi et une interface au choix | Voir §5.5 |
| Bindings Python | **nanobind** | pybind11 | La forge pilote le vrai moteur |
| Forge | **Python 3.12+**, NumPy, SciPy, PyTorch | JAX, Julia | Analyse et ajustement par gradient |
| Prototypage DSP | Python et NumPy ; Faust pour des filtres isolés | — | Itération rapide |
| Réseaux (optionnels, plus tard) | RTNeural (BSD-3) | ONNX Runtime | Seuls de petits modèles causaux tiennent 10 ms |
| Format de profil | TOML et binaires simples | FlatBuffers | Lisible et comparable en diff |
| Build et CI | CMake presets, CPM ou vcpkg, GitHub Actions sous Windows, macOS et Linux | — | Les trois plateformes testées à chaque commit |
| Qualité temps réel | RTSan (Linux, macOS), Tracy, ASan et UBSan, tests de charge | — | Détecter les violations avant qu'elles ne s'entendent |

**Un détail utile pour la forge.** La partie linéaire du modèle modal a une **forme close** : une somme de sinusoïdes amorties. Elle se différencie facilement et se calcule en parallèle sous PyTorch. Seul le marteau, non linéaire et actif quelques millisecondes, demande une récurrence différentiable. La calibration par gradient est donc beaucoup plus simple que pour un guide d'onde.

### 5.4 C++ ou Rust : le vrai arbitrage

Les deux langages satisfont les contraintes dures : pas de ramasse-miettes, du SIMD, du multiplateforme. La différence est ailleurs.

- **C++ a l'écosystème.** Toutes les références des deux documents sont en C++. Faust génère du C++. Les SDK de plugins, JUCE et les outils temps réel (RTSan, Tracy) sont en C++.
- **Rust a la sûreté et l'outillage** : cargo, et PyO3 pour Python. Mais sa couche plugin est en mouvement (nih-plug en maintenance, clack bas niveau), et AU y reste difficile.
- **Paradoxe.** Le moteur DSP se résume à quelques milliers de lignes de boucles chaudes, où la sûreté mémoire apporte relativement peu. C'est la coquille (threads, fichiers, interface) qui en profiterait le plus, et c'est justement là que l'écosystème audio de Rust est le plus faible.

**Recommandation : C++20**, sauf si tu es nettement plus à l'aise en Rust. Dans ce cas, le moteur passe en Rust derrière une API C, l'application autonome aussi (cpal, midir, egui), et le plugin vient plus tard, soit par une fine coquille C++, soit par le fork de nih-plug. Dans les deux cas, l'API C garde la décision réversible pendant les premiers mois.

### 5.5 JUCE ou « sans framework »

[R] recommande CLAP et clap-wrapper sans framework, pour garder un produit fermé sous licences permissives. Pour le premier objectif de PianoForge, une application pour jouer, enregistrer et régler, **JUCE fait gagner plusieurs mois de plomberie**. Il gère la sélection des périphériques sur les trois systèmes (ASIO compris), le MIDI, les fichiers WAV, FLAC et MIDI, les wrappers autonome, VST3 et AU, et l'interface.

**La licence a été vérifiée.** JUCE est disponible sous AGPLv3 ou sous licence JUCE. L'offre Starter est gratuite jusqu'à 20 000 $ de revenus annuels issus des produits JUCE, autorise le code fermé et n'impose plus d'écran de démarrage depuis JUCE 8. L'offre Indie va jusqu'à 300 000 $. Pour un usage personnel ou open source, il n'y a aucune contrainte.

**Garde-fou : aucun `juce::` dans le cœur.** On ne choisirait le « sans framework » que pour un produit commercial fermé qui dépasserait le plafond Starter sans vouloir payer de licence. Côté ASIO, une application JUCE sous licence commerciale utilisera la licence ASIO propriétaire, qui est gratuite.

---

## 6. Données de calibration et banc de référence

### 6.1 Les sources

| Source | Instrument | Intérêt | Licence |
|---|---|---|---|
| **Noire**, sous Kontakt 8, **installée sur ta machine** | Yamaha CFX | Référence locale de haute qualité, rendue à volonté par le banc (§6.2) | Licence (EULA) Native Instruments : usage interne seulement (voir plus bas) |
| **bitKlavier Grand** (Princeton) | Steinway D ; 16 couches ; 9 paires stéréo dont une binaurale ; 96 kHz/24 bits ; relâchements et pédales | **Candidat n°1 pour un premier profil « concert 274 cm »** | À vérifier avant tout usage |
| Salamander et Accurate-Salamander | Yamaha C5 ; 16 couches ; 48 kHz | Développement et second profil ; paramètres par note publics | CC-BY 3.0 |
| Iowa MIS | Steinway B ; pp, mf et ff ; son sec | Analyse de B et des décroissances | « Freely available », sans licence explicite |
| BiVib | Deux Disklaviers ; 10 vélocités | Vélocités répétables | À vérifier |
| MAESTRO | Disklavier en salle | À éviter pour la calibration note par note (réverbération de salle, licence non commerciale) | CC BY-NC-SA |
| Tes propres enregistrements | Un vrai piano à queue | Le seul moyen de viser un instrument précis | — |

Aucune donnée publique n'a été identifiée pour le Kawai SK-EX ni pour le Yamaha CFX. Pour le CFX, Noire fournit une référence d'usage interne ; pour le reste, il faudra des enregistrements. Les licences des banques commerciales interdisent souvent l'analyse et la dérivation, et sont à lire avant tout usage.

### 6.2 Le banc de référence : faire jouer Kontakt automatiquement

**Ce qui est installé.** Kontakt 8 existe en VST3 (`C:\Program Files\Common Files\VST3\Kontakt 8.vst3`) et en application autonome. Noire est installée dans `C:\Users\Public\Documents\Noire Library`. On dispose donc localement d'un piano de concert échantillonné de haute qualité.

**Le principe.** Un outil Python décrit une **séance de mesure** : quelles notes, quelles vélocités, quelles pédales, quelles durées. Il la fait jouer par Kontakt et range chaque rendu avec ses métadonnées. Le même protocole sera ensuite rendu par PianoForge, d'où des paires directement comparables. Le banc n'est pas un outil jetable : c'est **le banc de comparaison permanent du projet**. Il sert aussi plus tard à mesurer la latence, ou à enregistrer un vrai piano numérique ou un piano reproducteur.

```
séance.toml ──► protocole ──► prises (événements MIDI datés à l'échantillon près)
                                 │
          ┌──────────────────────┼───────────────────────┐
          ▼                      ▼                       ▼
  rendu hors ligne        rendu temps réel          rendu PianoForge
  (Kontakt VST3 hébergé   (port MIDI virtuel →      (pianoforge-cli,
   par pedalboard)         Kontakt autonome →        même protocole)
                           capture en loopback)
          └──────────────────────┼───────────────────────┘
                                 ▼
        corpus/<instrument>/<date>/ : FLAC + JSON par prise, index, manifeste
                                 ▼
                vérifications, puis métriques comparées
```

**Deux modes de rendu**

| | Hors ligne (recommandé) | Temps réel (secours et contrôle) |
|---|---|---|
| Principe | Kontakt VST3 est chargé dans un hôte Python sans interface (pedalboard, ou DawDreamer), et le MIDI lui est injecté directement | Un port MIDI virtuel (loopMIDI, ou le bouclage de Windows MIDI Services) attaque Kontakt autonome ; sa sortie est capturée en loopback WASAPI |
| Vitesse | Plus rapide que le temps réel | Temps réel strict : environ 10 h pour la passe principale |
| Calage temporel | Exact à l'échantillon : on sait quand chaque note part | À recaler par détection d'attaque |
| Fidélité | Celle du moteur Kontakt, sans passer par le mixeur du système | Risque de rééchantillonnage ou d'effets ajoutés par le mixeur Windows |
| Risques | Kontakt charge ses échantillons en différé : le rendu sort silencieux ou tronqué si l'on n'attend pas. Le streaming disque (DFD) peut décrocher hors temps réel. | Bruits du système capturés, dérive d'horloge |
| Usage | Constitution du corpus de référence | Valider le mode hors ligne sur une dizaine de notes ; mesurer la latence |

**Constats mesurés** (Kontakt 8.14.1, pedalboard 0.9.26) :

- **Le chargement** de Kontakt prend de 45 à 130 s.
- **Une remise à zéro** du plugin coûte exactement le délai d'initialisation passé à pedalboard : 60 s pour un délai de 60 s, 1,1 s pour un délai de 1 s. Le banc ne remet donc jamais Kontakt à zéro. Entre deux prises, il coupe le son (CC120, CC123) et attend le silence numérique.
- **Hébergé par pedalboard, Kontakt tourne toujours à 44,1 kHz**, quelle que soit la fréquence demandée (mesure du 5 novembre 2026 : un C4 rendu à 48 kHz sort à 284,7 Hz, à 96 kHz à 569,4 Hz). Le premier corpus, rendu à 48 kHz, était donc **1,47 demi-ton trop aigu**, avec des décroissances 8,8 % trop rapides. Les séances sont désormais à 44,1 kHz, et le banc refuse toute autre fréquence avec Kontakt. Il vérifie aussi la hauteur de A4 avant chaque rendu, et s'arrête si elle s'écarte de plus de 30 cents.
- **Sans instrument chargé**, le rendu va à plus de 1 000 fois le temps réel, et les longueurs de rendu sont exactes à l'échantillon près.
- **Avec Noire** (état « noire-dry » = snapshot Basic Pure, séance d'essai de 20 prises, 4 novembre 2026) :
  - le rendu va de **47 à 78 fois le temps réel**, si bien que la séance de référence complète (10 h 47 d'audio) se rend en un quart d'heure environ ;
  - le rendu rapide est **identique bit à bit** au rendu cadencé au temps réel : le streaming disque suit, et le rendu est déterministe ;
  - un fichier stocké ne diffère d'un nouveau rendu que par l'arrondi du FLAC 24 bits ;
  - les fins de prise retombent jusqu'au silence numérique, ce qui exclut toute réverbération ; aucune attaque n'est ajoutée, ce qui exclut Particles. Après le relâchement, le niveau perd 40 dB en 0,9 à 2,2 s ; C8, sans étouffoir, continue de sonner ;
  - les crêtes vont de −50 dBFS (C8, vélocité 20) à −11,6 dBFS (A0, vélocité 127). Il n'y a aucun risque de saturation.
- **Comportement de Noire avec la pédale** (séance des interactions, état « noire-dry », `studies/noire_pedal_behavior.py`) :
  - **Résonance sympathique : présente.** Touche tenue, la version pédale enfoncée porte 0,5 à 4,4 dB d'énergie en plus sous 4 kHz. Elle est très proche de la version pédale relevée juste après l'attaque (−12 à −30 dB d'écart), puis s'en écarte à mesure que la résonance se construit. Dans l'aigu, elle garde une énergie surtout grave après l'extinction de la note : les cordes graves libérées vibrent par sympathie. Noire contient donc des prises pédale enfoncée ou une résonance équivalente, ce qui en fait une référence valable pour cet étage du moteur.
  - **Demi-pédale : progressive.** Par rapport à la même note tenue sans pédale, l'étouffement commence dès que la pédale remonte un peu (−1 dB à CC64 = 104). Il s'accentue jusqu'à CC64 ≈ 16 (−8 dB sur C2, −13 dB sur C4), puis l'étouffoir se referme complètement entre 8 et 0 (−50 dB). C'est la référence des étouffoirs continus.
  - **Bruit de pédale : absent** (silence numérique) avec Basic Pure.
  - **« All Resonance »** n'est pas un réglage de résonance des cordes : c'est un preset de couleur sonore, qui pousse la réverbération au maximum. Il est écarté.
- **Le lecteur de l'atelier comparé à Noire sur une pièce entière** (« Study No. 1 », 180 notes, `studies/compare_renders.py`, 5 novembre 2026) :
  - **Étouffoirs.** Ils sont mesurés par la forge (`forge release-curves`) en comparant les prises `releases` et `single-notes`, qui sont identiques au bit près jusqu'au relâchement. Une note fraîche perd 7 à 14 dB en 100 ms, et rien ne se passe au-dessus de A6. L'ancien modèle, une seule exponentielle réglée sur des notes tenues longtemps, était trois à quatre fois trop lent. C#6 non étouffé dans Noire est écarté comme anomalie de la bibliothèque.
  - **Bug de 50 ms corrigé.** Le lecteur jouait les 50 ms de pré-roll des prises, ce qui ajoutait 50 ms de latence à chaque note.
  - **Résultat.** L'écart moyen d'enveloppe passe de 2,4 à **0,9 dB** sur la pièce, et de 6,6 à **1,1 dB** dans le passage staccato. Le test d'annulation descend de +2,3 à −4,9 dB.
  - **Limites connues.** Les sons de relâchement de Noire (bruit de touche et d'étouffoir), qui dominent après une longue tenue, ne sont pas reproduits par une courbe de gain. Les tenues longues et le relâchement de pédale s'étouffent 2 à 6 dB trop lentement dans le grave. Ces écarts restent vers −90 dBFS.

**La préparation de Kontakt, une seule fois**

1. Au premier lancement, l'outil ouvre l'éditeur de Kontakt. On y charge Noire, on règle, puis on ferme. L'outil **sauvegarde l'état complet du plugin** (bloc binaire et son hash) dans le manifeste du corpus, et tous les rendus repartent de cet état.
2. Noire est réglée pour une référence propre, dans la mesure où la bibliothèque le permet. On désactive Particles et la variante Felt, et on coupe réverbération et effets. On choisit une seule perspective de micros, la plus proche, ou bien on fait un corpus par perspective. La courbe de vélocité est linéaire. Le volume laisse de la marge au fff, à vérifier sur la crête. L'état « sec », utilisé par la séance de référence, coupe la résonance sympathique et les bruits mécaniques (touches, pédale). Il garde les sons de relâchement, qui font partie de la fin de la note. Un second état rétablit le tout pour la séance des interactions.
3. Dans Kontakt, le préchargement est porté au maximum, ou le streaming disque désactivé, pour fiabiliser le rendu hors ligne.

**Le protocole de mesure**

| Passe | Contenu | Volume estimé |
|---|---|---|
| **Découverte des couches** | 6 notes (A0, C2, C3, C4, C6, C8) × les 127 vélocités × 6 s | ≈ 1,3 h d'audio |
| **Notes isolées** (passe principale) | 88 notes × 16 vélocités. Tenue adaptée au registre (≈ 35 s dans le grave, 20 s dans le médium, 10 s dans l'aigu), plus 3 s après le relâchement | 1 408 prises, ≈ 9 h d'audio. La séance de référence complète (10 h 47) pèse 1,2 Go en FLAC 24 bits (mesuré). |
| Relâchements | 22 notes (une sur quatre) × 3 vélocités × tenues de 0,1, 0,5 et 2 s | Faible |
| Pédale forte | Même note pédale enfoncée et pédale relevée (sympathie) ; pédale relâchée pendant la note | Faible |
| Demi-pédale | Rampes de CC64 sur une note tenue | Faible |
| Refrappe | Notes répétées à 50, 100, 200 et 500 ms d'intervalle | Faible |
| Una corda (CC67) | Seulement si la bibliothèque la gère | Faible |
| Bruits seuls et silence | Pédale sans note, bruit de fond | Faible |

**Pourquoi découvrir les couches.** Une banque d'échantillons n'a qu'un nombre fini de couches de vélocité. Entre deux couches, elle fond ou filtre. En rendant les 127 vélocités sur quelques notes, on voit apparaître les sauts spectraux, donc les frontières des couches. Pour calibrer, on utilise ensuite les vélocités au centre des vraies couches, et on sait lesquelles sont interpolées.

**Les vérifications automatiques**

- Chaque prise contient une attaque et n'est pas silencieuse.
- Aucune prise n'écrête.
- Le rendu est déterministe : environ 5 % des prises sont rendues une seconde fois et comparées. Une différence trahit du round-robin ou des variations aléatoires ; il faut alors les désactiver, ou rendre chaque prise plusieurs fois.
- Le rendu hors ligne est comparé à une capture en temps réel sur une dizaine de notes.
- La queue de chaque prise descend sous −60 dB ; sinon, la tenue est allongée.
- Les métadonnées sont complètes.

**Une génération longue, donc reprenable.** La passe principale est la « phase de génération longue » du projet : elle peut tourner une nuit. L'outil saute les prises déjà rendues, repérées par le hash de la prise et de l'état de Kontakt. Il journalise et affiche sa progression.

**Ce que vaut cette référence, et ce qu'elle ne vaut pas**

- Elle est fiable pour **le timbre des notes isolées** : l'attaque, le spectre selon la vélocité, l'inharmonicité, l'accord, les décroissances et les battements. C'est ce que les échantillons captent bien.
- Elle l'est moins pour **les interactions** : sympathie, demi-pédale, refrappe. La bibliothèque les modélise ou les approche elle-même.
- La vélocité MIDI de Noire n'est pas une vitesse de marteau. La correspondance entre les deux est inconnue et devra être calibrée.
- Les rendus intègrent la couleur des micros et de la salle. Il faut comparer à perspective égale, ou s'appuyer sur des grandeurs robustes : B, f0, T60 par partiel, spectres normalisés.
- C'est donc **une référence de timbre, pas une vérité physique**. On la complète par Iowa (son sec) et bitKlavier.

**La licence.** L'EULA de Native Instruments autorise l'usage des sons dans des productions musicales. Elle interdit de les redistribuer ou d'en tirer une autre banque ou un autre produit sonore. Le corpus reste donc **local et hors de git**. Dans le POC personnel, il sert de cible de calibration et de comparaison A/B. Un profil destiné à être publié devra être recalibré sur des données à licence compatible.

**La pile technique du banc**

- **Python 3.11**, déjà installé.
- **pedalboard** (GPL-3.0) pour héberger le VST3, ou **DawDreamer** (GPL-3.0) en alternative. Ces deux licences ne posent pas de problème : le banc est un outil de développement séparé, jamais lié au cœur ni distribué avec lui.
- **mido** (MIT) pour le MIDI, **soundfile** (BSD) pour le FLAC, **NumPy**.
- En temps réel : **python-rtmidi** (MIT), **loopMIDI** (gratuiciel) ou le bouclage de Windows MIDI Services, et une capture loopback WASAPI (`soundcard` ou PyAudioWPatch).
- En dernier recours, si l'hébergement hors DAW de Kontakt s'avère instable : un rendu hors ligne scripté dans REAPER.

**L'arborescence, telle qu'implémentée** (le code est en anglais, voir le §9)

```
tools/bench/
  pyproject.toml
  bench/protocol.py        séance TOML → liste de prises (événements datés)
  bench/kontakt.py         hôte VST3 hors ligne, état sauvegardé, attente du chargement
  bench/storage.py         FLAC + JSON par prise, index, manifeste, reprise
  bench/checks.py          silence, écrêtage, attaque, notes parasites, décroissance, écarts entre rendus
  bench/keyboard_probe.py  sonde du clavier MIDI
  bench/cli.py             list | prepare | render | check | reanalyze | summary | keyboard
  sessions/reference.toml, interactions.toml, smoke.toml
corpus/                    (hors git)
  states/noire-dry.bin     état complet de Kontakt (et .vstpreset, .json)
  reference/noire-dry/
    manifest.json          empreinte de l'état Kontakt, fréquence, versions des outils, séance d'origine
    single-notes/060-C4_v064_h20000ms.flac + .json
    index.csv
```

Le mode temps réel (port MIDI virtuel et capture en loopback) n'a pas été écrit : le contrôle a montré que le rendu hors ligne est identique bit à bit à un rendu au rythme réel.

---

## 7. Risques

| Risque | Gravité | Parade |
|---|---|---|
| Réalisme plafonné par les données | Élevée | Noire pour comparer et bitKlavier pour calibrer ; tester la chaîne de calibration tôt (spike B) |
| Kontakt instable ou silencieux hors DAW | Moyenne | Attendre la fin du chargement, précharger au maximum, valider contre le mode temps réel ; DawDreamer, puis REAPER en secours |
| Usage des rendus de Noire hors du cadre de l'EULA | Moyenne | Corpus local et hors git ; comparaison seulement ; aucun profil diffusé calibré sur Noire |
| Ampleur du projet pour un développeur seul | Élevée | Un jalon jouable à chaque phase ; un périmètre V1 strict |
| Attaque peu crédible, point faible n°1 de tous les tests d'écoute | Élevée | Résidu d'attaque mesuré dès la phase 2 |
| Latence sous Windows | Moyenne | ASIO ou WASAPI exclusif ; mesure intégrée |
| Précision numérique | Faible, une fois traitée | Résonateurs complexes, FTZ/DAZ |
| Écart entre le modèle Python et le moteur C++ | Moyenne | La forge pilote le vrai moteur ; tests miroir |
| Brevets (US7915515B2, expiration estimée au 20 octobre 2027 et éventuels équivalents européens à vérifier ; Yamaha JP7476501B2) | Faible en usage privé, élevée en diffusion commerciale | L'usage privé et non commercial relève de l'exception de l'article L613-5 du Code de la propriété intellectuelle (ce n'est pas un avis juridique). Consulter un conseil en propriété industrielle avant toute vente. |
| Marques (Steinway, Kawai…) | Même profil que les brevets | Des noms neutres (« concert 274 cm ») en cas de diffusion |
| Licences (GPL de FigBug/Piano, AGPL d'OpenPiano et de JUCE) | Moyenne | Lire ces codes sans les copier ; garder un cœur permissif |

---

## 8. Feuille de route revue pour un développeur seul

**D'abord, quatre spikes de dérisquage**, d'une à deux semaines chacun. Chacun répond à une question précise avant d'engager le reste. Le spike 0 vient en premier, parce que tous les autres s'appuient sur ses rendus.

| Spike | Question | Livrable | Critère de réussite |
|---|---|---|---|
| **0. Banc de référence** | Peut-on faire jouer Noire automatiquement, de façon fiable et reproductible ? | `tools/bench` (§6.2) : préparation, rendu hors ligne, vérifications ; passes de découverte des couches et de notes isolées | Dix notes hors ligne conformes à la capture en temps réel ; déterminisme vérifié ; passe principale menée en une nuit |
| **A. Latence** | Ta machine et ton clavier tiennent-ils 10 ms et 1 ms de gigue ? | Application JUCE minimale (MIDI vers un clic), mesure au micro | Latence et gigue mesurées, pilote choisi |
| **B. Une note** | Un modèle modal calibré sur un enregistrement sonne-t-il juste ? | Notebook Python : f0, B et décroissances d'un C4 (Noire par le banc, Iowa), resynthèse, écoute A/B | Partiels et T60 dans les tolérances perceptives (−25 % à +40 %) |
| **C. Débit** | Combien de résonateurs par cœur sur ta machine ? | Banc C++ avec Highway : 22 000 résonateurs complexes en blocs de 64 | Pire cas sous 30 % d'un cœur |

**Où en est le spike 0 (4 novembre 2026).** Le banc est écrit (`tools/bench`) et couvert par 17 tests. L'état « noire-dry » est préparé, et le contrôle est **conforme** sur la séance d'essai (§6.2). La séance de référence complète se rend en 15 minutes. Il reste à préparer un second état, avec résonance et bruits, pour la séance des interactions. La sonde du clavier (`bench keyboard`) attend que le B2 soit branché.

**Spike B réalisé (5 novembre 2026, `studies/spike_b_modal_note.py`).** La forge analyse les 88 notes de Noire (`forge analyze`) : inharmonicité B, courbe d'accord étiré de Railsback, double décroissance. Elle écrit le premier profil `physics.toml`. A1, C4 et C6 ont été resynthétisés avec un modèle modal, où chaque partiel est une paire de sinusoïdes amorties (prompt sound et aftersound) mesurées sur la prise.
  - **Résultat.** Après l'attaque, le niveau est juste à **±2 dB** dans chaque fenêtre de temps, et la distance spectrale (insensible à la phase) est de **4 à 6 dB**. Le modal seul rate l'attaque (+5 dB, 6 à 12 dB de distance sur 100 ms) ; l'hybride avec le résidu d'attaque mesuré la reproduit.
  - **Ce qui manque**, visible sur les spectrogrammes : les **battements** entre les cordes d'un même chœur, le modal étant trop lisse et donc « numérique » ; et le **corps de la table d'harmonie**, une énergie sous la fondamentale (80 à 400 Hz) qui dure 1 à 2 s après la frappe. Il faut donc, comme prévu au §4.3, 2 à 3 cordes couplées et désaccordées par note, et un étage de table d'harmonie excité par le marteau.
  - **Leçon de méthode.** Une différence échantillon par échantillon ne juge pas une resynthèse : quelques centièmes de hertz d'écart suffisent à faire dériver la phase. Il faut des mesures insensibles à la phase, comme le niveau par fenêtre et la distance entre spectrogrammes.

**Spike B v2 : battements et table d'harmonie (5 novembre 2026, `forge modes`, `forge soundboard`, `studies/spike_b2_unisons.py`).**
  - **Les cordes.** Chaque partiel est ramené en bande de base : la bande est découpée dans une longue FFT, avec une bande passante plate. Il est ensuite décomposé en 1 à 6 modes amortis. ESPRIT fournit un point de départ, puis une projection variable affine les pôles (Levenberg-Marquardt) et résout exactement l'amplitude complexe de chaque mode par canal. L'ordre retenu est le plus petit qui reproduit l'enveloppe du partiel, battements compris. ESPRIT seul laissait 3 à 12 dB d'erreur d'enveloppe ; l'affinage la ramène à **1 à 2 dB** (médiane par note, sur les 88 notes à la vélocité 64, en 5 minutes sur 12 cœurs).
  - **Le cordage se lit dans les données.** On mesure environ 2 modes par partiel de A0 à E1 : des cordes simples, avec leurs deux polarisations. Au-dessus, on en mesure 3 à 5, pour 2 puis 3 cordes. Les unissons s'étalent sur quelques cents. Le fondamental de C4 commence sur un mode proche de 0 Hz, puis finit sur le mode de l'aftersound, à +0,2 Hz : c'est le mécanisme de Weinreich.
  - **Noire est débruitée.** Les partiels disparaissent brutalement vers −140 dB, et il reste autour d'eux un bruit plus fort que dans les intervalles. La fenêtre d'analyse s'arrête donc au plancher de bruit (+15 dB) et 80 dB sous le partiel le plus fort. Un T60 plus long que dix fois la fenêtre n'est pas mesuré mais extrapolé : il est plafonné.
  - **La table d'harmonie.** Une fois les cordes retirées, il reste sous la fondamentale un corps large bande (30 à 700 Hz) qui s'éteint en une seconde environ. La physique prévoit des résonances propres à l'instrument, dont seule l'excitation dépend de la note. La mesure le confirme : des pôles estimés sur les notes aiguës impaires expliquent aussi bien les notes paires (−18 à −22 dB sur la première demi-seconde, selon le nombre de pôles). Le modèle retenu compte **200 résonances partagées** (T60 médian 0,64 s), estimées par ESPRIT multicanal sur les 24 notes les plus aiguës. Les amplitudes de chaque note sont ajustées par moindres carrés dans le domaine fréquentiel, en excluant les bandes des partiels. Une résonance qui tombe sous un partiel n'est pas mesurable : elle reste à zéro, et le partiel la masque de toute façon.
  - **Résultat** (v1 = spike B ; v3 = modes + table + 100 ms de résidu d'attaque) :

| Note | Erreur d'enveloppe des partiels, v1 → v3 | Niveau du corps (0,25–1 s / 1–2,5 s), v1 → v3 | Distance spectrale 0,1–1 s, v1 → v3 |
|---|---|---|---|
| A0 | 3,7 → **0,6 dB** | — (fondamentale trop basse) | 2,5 → 1,1 dB |
| A1 | 4,4 → **0,7 dB** | — | 3,8 → 0,5 dB |
| C4 | 7,5 → **1,3 dB** | −43 / −81 → **−0,7 / −3,1 dB** | 4,6 → 1,3 dB |
| C6 | 4,3 → **1,7 dB** | −46 / −64 → **−1,2 / +3,5 dB** | 4,3 → 3,4 dB |
| C7 | 3,9 → **1,2 dB** | −46 / −70 → **−0,8 / −1,0 dB** | 5,5 → 7,3 dB |

  - **Limites.** Sur C7, le transitoire dure plus longtemps que les 100 ms de résidu d'attaque, au-dessus de la bande de la table. La queue du corps (au-delà de 0,5 s) n'est juste qu'en niveau moyen. Dans le grave, le corps reste masqué par les partiels et n'est pas modélisé. Tout est mesuré à une seule vélocité.
  - **La linéarité en vélocité** a d'abord été testée sur trois vélocités avec une fenêtre commune, ce qui biaisait le test. Elle est tranchée plus bas, couche par couche.
  - **La suite.** Le modèle physique des cordes couplées (§4.1, la cuisson) prend ces modes mesurés pour cible. Il en tire les désaccords d'unisson et l'admittance du chevalet, ce qui donne des réglages de technicien, l'una corda et des vélocités continues. À l'écoute, l'utilisateur juge la version « modes + table + résidu de 100 ms » « super propre ».

**Test des vélocités (5 novembre 2026, `studies/velocity_linearity.py`).** Les 12 premiers partiels de A0, A1, C4, C6 et C7 sont analysés sur chacune des 16 couches de vélocité, chaque couche sur sa propre fenêtre. Le tableau donne le coût en erreur d'enveloppe par rapport à l'ajustement indépendant de chaque couche (médiane / 90e centile, en dB).

| Variante | A0 | A1 | C4 | C6 | C7 |
|---|---|---|---|---|---|
| Pôles de v64, amplitudes par couche | +0,5 / +7,1 | +0,7 / +4,6 | +1,0 / +4,7 | +1,9 / +6,1 | +1,0 / +2,8 |
| Pôles communs, ajustés sur toutes les couches | 0,0 / +0,9 | +0,3 / +1,5 | +0,5 / +1,9 | +0,9 / +3,1 | +0,8 / +3,6 |
| Modes appariés, valeurs propres à chaque couche | −0,1 / 0,0 | −0,1 / +0,2 | 0,0 / +0,9 | −0,1 / +0,4 | −0,1 / +1,7 |
| Couche prédite par ses voisines : pôles interpolés | 0,0 / +1,2 | 0,0 / +1,1 | +0,6 / +2,8 | +0,6 / +3,1 | +1,0 / +3,5 |
| … pôles et amplitudes complexes interpolés | +0,8 / +6,2 | +0,9 / +7,3 | +2,6 / +20,8 | +3,2 / +8,5 | +2,2 / +7,2 |
| … couche voisine, niveau interpolé par partiel | +1,4 / +6,4 | +1,2 / +7,2 | +1,9 / +13,5 | +3,4 / +14,9 | +2,9 / +32,8 |

  - **Les pôles bougent avec la vélocité.** Figés sur v64, ils coûtent jusqu'à 6 à 8 dB aux couches extrêmes. Des valeurs propres à chaque couche ne coûtent rien.
  - **Aucune couche ne se prédit à partir de ses voisines.** Chaque couche est une frappe enregistrée à part, avec ses propres phases de battement. Interpoler les amplitudes complexes est la pire solution : les modes s'annulent par endroits.
  - **Décision de format.** Le profil garde les modes de chaque couche tels que mesurés. À une vélocité intermédiaire, le moteur joue la couche la plus proche, qui est une vraie frappe, et interpole le niveau de chaque partiel entre les deux couches voisines : le niveau et le timbre suivent la vélocité sans à-coup. Une vélocité continue « vraie », c'est-à-dire une frappe nouvelle à chaque vélocité, viendra du modèle physique des cordes couplées.
  - **Ajustements ratés.** Sur les 16 couches, 0,1 % des modes (décroissance très rapide, amplitude à la frappe extrapolée de plus de 40 dB) et 1 % des partiels (erreur d'enveloppe de plus de 20 dB) sont des échecs de l'ajustement. La couche 64 n'en avait aucun. La forge les écarte (`note_modes.clean`), avant la table d'harmonie et l'export.

**Moteur modal v0 (5 novembre 2026, `engine/`, `forge export`).**
  - **Le profil** `modal-profile.bin` contient, pour chaque note et chaque couche, les modes de chaque partiel, les amplitudes des 200 résonances de la table et le résidu d'attaque (100 ms, fondu sur les 40 dernières), plus les courbes d'étouffoir. Comme le corpus, il reste local et n'est jamais publié.
  - **Les résonateurs** sont complexes, en float32, rangés en structure de tableaux. Le noyau est écrit en AVX2 avec FMA, et deux paquets de 8 modes sont entrelacés pour recouvrir la latence de la mise à jour des phaseurs. MSVC ne vectorisait pas la version en C++ simple, qui était 7 fois plus lente.
  - **Le moteur** joue une banque de modes par voix et une table d'harmonie partagée par toutes les notes, que chaque frappe excite. Il ajoute le résidu d'attaque et applique les courbes d'étouffoir mesurées, la pédale et la refrappe (fondu de 0,8 s, comme le lecteur de référence).
  - **Validation.** Le rendu C++ de C4 à v64 reproduit le rendu Python de la version v3 à −58 dB près, avec les mêmes métriques. Les tests unitaires du moteur portent sur un profil synthétique. L'autotest de l'application compare le niveau de C4 au corpus et mesure la charge.
  - **L'atelier** passe du lecteur de référence au moteur modal pendant qu'on joue, ce qui permet l'A/B au clavier. Les prises enregistrées se rendent avec le moteur choisi.
  - **Le profil complet** compte 88 notes × 16 couches et 122 020 modes. Il pèse 33,6 Mo et se charge en 0,08 s. Deux couches sans partiel exploitable sont écartées (A2 v56, C#3 v88). Quatre corps de table aberrants sont remplacés par celui de la couche médiane de la note, mis à l'échelle de ses cordes (`export.repair_bodies`) ; le pire, A0 à v40, était 130 fois trop fort.
  - **Le niveau** colle à Noire à ±0,5 dB près, par fenêtre de 0 à 0,1 s, de 0,1 à 1 s et de 1 à 3 s, sur 8 notes de A0 à C8 et 6 couches. Seules exceptions : les queues de C7 et C8 aux vélocités douces, qui sont à −92 à −130 dBFS, donc inaudibles.
  - **La charge**, mesurée sur la machine au repos : le noyau coûte 0,27 ns par mode et par échantillon, soit environ 77 000 résonateurs en temps réel sur un cœur. 64 notes pédale enfoncée (5 400 résonateurs à 48 kHz, en blocs de 64) prennent 9,4 % d'un cœur, avec un pire bloc de 294 µs sur 1 333 µs. Le critère du spike C (pire cas sous 30 % d'un cœur) est tenu sans le multicœur.
  - **Une phrase de 18 s** (`studies/ab_phrase.py`) met en jeu des notes isolées, un accord pédalé avec une mélodie, un arpège détaché, des refrappes et une octave grave forte. Entre les deux moteurs, le niveau coïncide (−37,7 dB RMS chacun). Section par section, l'écart reste sous ±1,1 dB et la distance spectrale entre 0,9 et 3,1 dB.

**Éclat aigu à l'attaque dans le grave (5 novembre 2026).** Au B2, l'utilisateur entend « un son de bulle aigu » sur les notes graves avec le moteur modal, mais pas avec Noire, ni à la souris. Il y avait deux causes, toutes deux à l'attaque.
  - **Le résidu et les gains par partiel.** Avant le début de leur fenêtre d'analyse, les modes sont extrapolés jusqu'à la frappe, et le résidu (prise moins modèle) annule cet excès. Il ne l'annule qu'au même gain. Or, entre deux couches, chaque partiel recevait son propre gain, et le résidu le gain global. L'annulation cassait et laissait un éclat dans l'aigu. Par exemple, l'attaque de E1 était à +7 à +10 dB au-dessus de la couche 72 à v69–71, entre 1,8 et 2,6 kHz. **Correctif dans le moteur :** pendant le résidu, tout (modes, table, résidu) porte le gain global, si bien que l'attaque est la vraie frappe de la couche la plus proche. Chaque partiel rejoint ensuite son niveau en 50 ms, par une rampe exponentielle obtenue en modifiant le module des pôles de ce partiel ; les pôles exacts sont ensuite rétablis. La table suit le gain global.
  - **Le résidu trop court dans le grave.** La fenêtre d'analyse des partiels commence d'autant plus tard que les partiels sont serrés : 0,186 s pour A0, 0,139 s pour C#1, 0,081 s à partir de C2. Le résidu de 100 ms, qui commence à s'effacer dès 60 ms, laissait donc de A0 à B1 un trou où les modes extrapolés jouaient seuls. Pour C#1 à v127, cela donnait +5 dB vers 1 kHz pendant 80 ms. **Correctif dans la forge :** le résidu dure jusqu'au début de la fenêtre des partiels, puis s'efface sur 40 ms, avec au moins 100 ms au total (`export.residual_seconds`). Le profil passe à 40,6 Mo.
  - **Mesure** (énergie par tiers d'octave au-dessus de 1 kHz, de 0 à 0,12 s ; 1 800 rendus de A0 à C4 aux vélocités entre les couches, comparés aux deux couches Noire voisines ; on ne compte que ce qui dépasse −50 dB sous le maximum de la note) :
    - cas à plus de 6 dB au-dessus des deux couches : 47 avant, 0 après ;
    - pire écart : +31,9 dB avant, +4,2 dB après ;
    - de A0 à B1 aux couches exactes, pire écart de 0 à 0,3 s au-dessus de 500 Hz : +10,0 dB avant, +3,2 dB après.

**Traînes et table (5 novembre 2026, profil version 2).** Deux défauts restaient là où les mesures sont extrapolées.
  - **Les partiels au-delà de leur fenêtre.** La fenêtre d'analyse d'un partiel s'arrête au dernier instant où il dépasse son plancher : 50 dB sous son maximum, le bruit + 15 dB, ou 80 dB sous le partiel le plus fort de la note. Au-delà, les modes ne sont plus contraints par la mesure.
    - Ils pouvaient sonner bien plus fort que la prise, de deux façons. Soit ils décroissaient lentement, en restant des dizaines de secondes près du plancher : pour B0 à v16, les partiels vers 800 Hz restaient à −42 dB sous le maximum de la note pendant 20 s, alors que Noire, débruitée, les fait disparaître 45 dB plus bas. Soit des modes voisins, qui s'annulaient sur la fenêtre, remontaient ensuite : le partiel 14 de B0 à v8 repartait de −40 à −17 dB.
    - **Correctif.** La forge mesure la prise dans la bande de chaque partiel, jusqu'à la fin de la tenue (`export.fade_times`). Si le modèle la dépasse de plus de 6 dB pendant plus d'une seconde, le partiel reçoit un instant d'extinction : la fin de sa fenêtre, où il est à son plancher. Le moteur lui applique alors un amortissement supplémentaire de −60 dB par seconde, en modifiant le module de ses pôles à l'échantillon près. Une fenêtre qui court jusqu'à la fin de la tenue n'est pas touchée.
    - Cela concerne 20 808 partiels, dans 1 374 couches sur 1 408 : Noire, débruitée, fait disparaître presque tous ses partiels une fois sous leur plancher.
    - Le format passe en version 2 (instant d'extinction par partiel) ; le chargeur lit encore la version 1.
  - **La table trop forte sur les notes douces.** L'ajustement de la table pèse surtout la fin de sa fenêtre (×148 à 2,5 s), où se mesurent les résonances lentes. Une résonance rapide y est donc à peine contrainte, et sur quelques prises douces elle sort bien trop forte : +15 dB à 703 Hz pour E2 à v8, +8 dB vers 126 Hz pour F3 à v8.
    - **Correctif.** La forge compare la table à la prise moins les modes, dans les cases de fréquence que l'ajustement utilise (loin des partiels), par tiers d'octave, dans les fenêtres 0,12–0,3 s, 0,3–1 s et 1–2,5 s (`export.limit_body`). Là où la table dépasse la prise de plus de 3 dB, les résonances de la bande y sont ramenées. Le résidu d'attaque est recalculé avec la table ainsi limitée.
    - Les couches normales ne bougent pas (C4, C2 à v64, A0 à v104, C3 à v127). 400 couches sur 1 408 ont au moins une bande ramenée, surtout aux vélocités douces. Là où la table compte, l'écart médian à Noire reste le même (−1,3 dB contre −1,2 avant).
  - **Mesures.**
    - *Tenues longues de A0 à E3 (512 couches, rendu complet de la tenue) :* les couches où le moteur dépassait Noire de plus de 10 dB pendant au moins 2 s passent de 208 à 1 ; pendant au moins 5 s, de 105 à 0.
    - *Attaques aux couches exactes de A0 à C4 :* le pire écart passe de +14,7 dB (E2 à v8) à +6,4 dB.
    - *Niveau total par fenêtre (0–0,1 s, 0,1–1 s, 1–3 s, 3–8 s), sur 30 notes et 6 couches :* l'écart médian à Noire reste sous 0,1 dB, comme avant. La phrase A/B reste à −37,7 dB pour les deux moteurs.
  - **Limites.**
    - Dans les notes aiguës très douces, il reste un excès de la table vers 32–40 Hz. Ce n'est pas une résonance : c'est l'éclaboussement de la coupure à −100 dBFS quand l'énergie de la table passe sous le seuil de silence. Il est inaudible, mais la coupure pourrait devenir un fondu.
    - Quelques queues aiguës s'éteignent dans le moteur alors que Noire garde un reste entre −92 et −130 dBFS.

**Pédale, étouffoirs, résonance sympathique et accordage (6 novembre 2026, profil version 3).** Après le test au B2 (« quasi parfait », bulle disparue), tout ce qui restait faisable sans le modèle physique de cordes couplées.
  - **Ce que fait Noire avec la pédale, mesuré.**
    - *Paires pédale enfoncée / relevée* (15 notes × 2 vélocités, touche tenue dans les deux cas). Noire joue le même échantillon et y ajoute une couche de résonance : −40 dB sous la note sur 0–0,1 s, −20 à −6 dB sur 0,5–1,5 s, −15 à −1 dB sur 1,5–4 s dans le grave et le médium. Elle sonne aux fréquences des cordes dont les partiels coïncident avec ceux de la note (pour A3 : A2, A4, E5), plus des cordes un peu désaccordées (217,5 et 435,8 Hz pour A3) ; rien au-dessus de 4 kHz.
    - *Sondes* (14 prises, passe `probes` de la séance interactions, écrites événement par événement grâce au nouveau type de passe `sequence` du banc ; Noire étant déterministe, chaque interaction est une différence de prises). Touches tenues sans pédale : **aucune** résonance (différence nulle au bit près). Pédale enfoncée après la note : la résonance apparaît, Noire la calcule donc à la volée. Pédale relevée touche tenue : elle s'éteint en 0,3 à 0,5 s. Pédale renfoncée 0,1 ou 0,3 s après un relevé : rien n'est rattrapé, l'étouffement va jusqu'au bout.
    - *Demi-pédale* (3 rampes de CC64 de 127 à 0) : de 127 à environ 16, les étouffoirs effleurent les cordes (la courbe d'étouffement court à 2 à 13 % de sa vitesse, −1 à −5 dB/s) ; à 12 et en dessous, ils reposent.
    - *Relâchement* (198 prises) : sous la courbe d'ensemble, les harmoniques aigus s'éteignent 10 à 30 dB plus vite que les graves dans le grave et le médium (A1 à 0,4 s : −18 dB pour la fondamentale, −35 dB vers 880 Hz). Noire ajoute un bruit de relâchement grave (centroïde vers 100 Hz, −75 à −95 dBFS sur une seconde) sur les notes étouffées, aucun sur la partie haute non étouffée.
  - **Résonance sympathique (moteur).** Chaque corde libre est une banque de résonateurs : ses modes mesurés (couche la plus proche de v64), 5 233 résonateurs sous 4 kHz sur 86 cordes. Ils sont excités par le son des autres voix (cordes et résidu d'attaque ; la table, commune, n'en fait pas partie). Une corde est libre quand la pédale lève son étouffoir, ou sous sa touche tenue. Un étouffoir qui retombe l'amortit comme le début de la courbe de la note (exponentielle tirée des 0,1 premières secondes). Coût : environ 10 % d'un cœur (banc de 64 voix à 48 kHz : 17,8 % du temps réel contre 8,3 % sans).
  - **Calibration** (`forge sympathetic`, sur les 30 paires). Par tiers d'octave, un gain de couplage et un amortissement ajouté à celui des cordes (sinon les cordes libres sonneraient des dizaines de secondes, alors que la couche de Noire meurt en quelques secondes). S'y ajoutent deux traits de Noire, pas d'un piano : sa résonance monte plus tard que des cordes ne répondraient (excitation en 1 − e^(−t/0,3 s), ce qui empêche aussi le choc du marteau de faire sonner toutes les cordes d'un coup), et elle est plus faible aux vélocités douces que la note elle-même (excitation en (v/64)^0,34). L'excitation de l'ajustement est la prise sans pédale elle-même, la note telle qu'entendue. L'exciter avec les seuls modes fait moins bien (écart médian +4,5 dB au lieu de +2,2) : extrapolés jusqu'à la frappe, ils exagèrent les premiers instants, que les cordes graves intègrent pendant des secondes.
    - *Dans le moteur*, sur les 30 paires et cinq fenêtres (0–0,2 s, 0,2–0,5 s, 0,5–1,5 s, 1,5–4 s, 4–9 s), la résonance (pédale enfoncée moins pédale relevée, rendues par le moteur) est à +1,2 dB de celle de Noire en médiane ; la moitié des fenêtres à moins de 4,7 dB, 90 % à moins de 11 dB. La phrase A/B garde le même niveau (−37,7 dB pour Noire, −37,6 dB pour le moteur).
  - **Au-delà de Noire, réglable dans l'atelier.** Les touches tenues font résonner leurs cordes (« Held keys resonate ») ; renfoncer la pédale rattrape ce qui reste des notes relâchées (« Pedal catches releases »). Un piano fait les deux, pas Noire.
  - **L'étouffoir piloté par la pédale.** La courbe d'étouffement d'une note est parcourue à une vitesse fixée par la pédale : 0 étouffoirs levés, 1 posés, entre les deux pour la demi-pédale (courbe mesurée par `forge half-pedal`). Rattrapage et demi-pédale passent ainsi par un seul mécanisme. Avec le B2, dont la pédale est un interrupteur, la demi-pédale ne joue que pendant les paliers de moins de 140 ms.
  - **Relâchement.** `forge release-curves` mesure aussi la courbe de chaque groupe de partiels (numéros 1, 2–3, 4–7, 8–15, 16–31, 32 et plus) relativement à la courbe d'ensemble. Le moteur l'applique aux pôles des partiels, bloc par bloc, en partant de leurs pôles exacts (rien ne s'accumule). Un groupe trop faible, ou plus fort que la note tenue (c'est alors le bruit de relâchement dans la bande d'un partiel faible), garde sa dernière valeur mesurée. `forge release-noise` ajuste le bruit sur les résonances de la table, loin des partiels ; le moteur l'ajoute à la table quand un étouffoir se pose. *Sur les 132 prises de relâchement (tenues de 0,5 et 2 s), l'écart médian par octave à Noire passe de 5,3 à 2,9 dB ; entre 0,6 et 1,2 s après le relâchement, de 16–18 dB à 2–4 dB dans les bandes de 220 à 1 760 Hz.*
  - **L'extrême aigu.** De C7 à C8, les modes manquent un tiers à la moitié du son bien au-delà de 0,1 s (3 à 10 dB sous la prise sur la première demi-seconde). Le résidu d'attaque se prolonge désormais tant qu'il reste à moins de 10 dB de la prise, jusqu'à 0,5 s (`export.quiet_from`). Le profil passe de 41 à 59 Mo.
  - **Accordage.** La de référence (415 à 466 Hz) et sept tempéraments (égal, Werckmeister III, Kirnberger III, Vallotti, mésotonique au quart de comma, pythagoricien, juste), sur n'importe quelle tonique, le La gardant sa fréquence. Toutes les fréquences de la note sont multipliées à la frappe (le résidu est lu plus vite ou plus lentement d'autant) ; les cordes libres sont réaccordées, pour que les coïncidences restent justes.
  - **Notes répétées.** Sur les 24 prises de répétition, l'enveloppe du moteur suit Noire à 0,5 dB (écart quadratique médian) quel que soit le fondu de la voix précédente, de 0,2 s à 30 s (la superposition physique) : la touche remonte entre deux frappes et l'étouffoir masque la différence. Le réglage reste à 0,8 s.
  - **Table.** Sous −100 dB, elle s'éteint en 20 ms au lieu d'être coupée.
  - **Format version 3.** Après les notes, des sections étiquetées (`SYMP`, `PEDL`, `DAMP`, `RELN` : tag, longueur, contenu) ; un lecteur ignore celles qu'il ne connaît pas. Le chargeur lit les versions 1 à 3.
  - **Limites.**
    - Un gain et un amortissement par bande, communs à toutes les notes, ne servent pas tout le clavier : dans le grave à v100, notre résonance reste 10 à 14 dB trop forte après 1,5 s (A0 à D#2) ; elle est trop faible sur A2 ; l'extrême aigu doux, après 4 s, est sous −100 dBFS des deux côtés.
    - L'étouffement d'ensemble est une moyenne des trois vélocités mesurées : à v100, A3 s'éteint 7 à 12 dB trop lentement 0,2 à 1 s après un relevé de pédale. Des courbes par vélocité sont possibles avec les mêmes prises.
    - Le grondement sous 55 Hz du bruit de relâchement de Noire (−80 à −100 dBFS) et son clic au-dessus de 7 kHz ne sont pas reproduits : la table ne couvre que 30–700 Hz.

**Phase 0 entamée.** L'application JUCE existe sous forme d'atelier d'écoute (`app/`) : clavier de 88 touches jouable à la souris, au clavier du PC (AZERTY ou QWERTY) et au clavier MIDI ; pédale forte ; choix de la sortie audio. Elle joue le corpus de référence derrière l'interface `pf::Engine` (`engine/`), celle que le moteur modal implémentera, ce qui permettra l'A/B. Un autotest sans interface (`PianoForge --selftest`) contrôle la fidélité de lecture, le rééchantillonnage, le relâchement et la charge.

**Capture des prises (5 novembre 2026).** L'hôte audio enregistre en permanence, sans verrou, chaque événement qui atteint le moteur, à l'échantillon près. « Save last take » garde ce qui a été joué depuis le dernier silence de 10 s. La prise est écrite en SMF, avec un tick par échantillon (tempo 120, ticks par noire = fréquence / 2), et les pédales déjà enfoncées sont rappelées au début. Un rendu WAV est fait en arrière-plan avec les réglages entendus, dans `Documents/PianoForge/Takes`. L'autotest joue une séquence par l'hôte (accord, pédale, refrappe d'une note tenue), l'exporte, la relit, la re-rend, et retrouve **l'audio en direct au bit près**. Le critère de sortie « une prise re-rendue à l'identique » de la phase 0 est donc tenu pour le lecteur de référence.
  - **Piège JUCE.** `MidiFile::readFrom` et `MidiMessageSequence::updateMatchedPairs` ajoutent par défaut un note-off avant toute refrappe d'une note encore tenue. Le moteur reçoit alors un relâchement (étouffoir) au lieu d'une refrappe. La prise doit être écrite et relue telle quelle (`readFrom(in, false)`, sans `updateMatchedPairs`).

**Enregistrement en direct (6 novembre 2026).** Un bouton Record / Stop (Ctrl+R), avec deux cases MIDI et WAV, enregistre ce que joue la carte son, en ASIO comme en WASAPI (l'ASIO était déjà actif dans l'atelier).
  - *Fichiers.* Dans `Documents/PianoForge/Recordings`, sous un nom horodaté (`PianoForge 2026-11-06 10-12-33`). Le WAV est en 32 bits flottants, avec un en-tête Broadcast WAV (date, heure, logiciel). Le MIDI a un tick par échantillon, et la pédale déjà enfoncée est rappelée au tick 0. Le tick 0 du MIDI est l'échantillon 0 du WAV : les deux sont alignés à l'échantillon.
  - *Aucune latence ajoutée.* Le chemin audio ne change pas. Après le moteur, le fil audio copie seulement son bloc dans une FIFO sans verrou, préallouée (10 s) : ni verrou, ni allocation, ni accès disque. Un fil séparé l'écrit sur le disque toutes les 20 ms. Départ et arrêt tombent sur des frontières de bloc, dont la position dans la session audio est gardée ; le MIDI de la même plage vient de la capture permanente des événements.
  - *Autotest.* Un enregistrement démarré et arrêté depuis un autre fil pendant que les rappels audio tournent donne un WAV identique, échantillon par échantillon, à l'audio envoyé à la carte, et un MIDI aligné (pédale rappelée, notes à leur place).

**Sortie pour le streaming (6 novembre 2026).** En ASIO, le son va droit au pilote de la carte : le mixeur de Windows ne le voit pas, et OBS ne peut le capturer ni par application ni par sortie. L'atelier a donc une seconde sortie, « Stream output (OBS) » : une copie de ce que joue la carte principale, envoyée par le mixeur partagé de Windows (WASAPI, en mode faible latence quand le périphérique l'offre ; ALSA sous Linux) vers un câble virtuel, dont OBS prend l'autre extrémité comme entrée audio (ou la capture de l'application).
  - *L'ASIO n'est pas touché.* Son fil copie seulement son bloc dans une FIFO sans verrou ; le miroir tourne sur son propre fil, avec sa propre latence (environ 40 ms jusqu'au câble : la cible de la FIFO, un bloc de 10 ms et la latence de sortie).
  - *Deux horloges.* Deux cartes ne tournent jamais exactement à la même fréquence (des dizaines à des centaines de ppm d'écart) : à rapport fixe, la FIFO se viderait ou déborderait en quelques minutes. Le miroir rééchantillonne par un sinus cardinal fenêtré (16 points, fenêtre de Kaiser, −85 dB d'erreur sur un sinus de 1 kHz), au rapport nominal corrigé par une boucle PI sur le remplissage de la FIFO moyenné sur une seconde (les blocs des deux côtés en font une dent de scie). *Autotest, 2 min simulées :* de 44,1 à 48 kHz avec 150 ppm d'écart, correction entre −160 et −134 ppm (−150 nécessaires, soit 0,03 cent), FIFO à sa cible, aucun décrochage une fois calé, sinus à −65 dB ; de 44,1 à 44,1 kHz avec −80 ppm, −79 dB.
  - *Vérification de bout en bout* (`PianoForge --mirror-check "<sortie>" "<entrée>" <rapport>`) : un son de 1 kHz à −43 dBFS passe par le miroir et revient par l'autre extrémité du câble. Avec le VB-Audio Hi-Fi Cable de la machine, il arrive au bon niveau mais à 929 Hz et haché : le câble était réglé à 48 kHz en lecture et 44,1 kHz en enregistrement, et il ne convertit pas entre ses deux extrémités. Les deux doivent avoir la même fréquence.

**Plugin OBS (6 novembre 2026).** Plus direct que le câble : une source audio « PianoForge » dans OBS (`obs/`), qui lit l'application elle-même, sans câble virtuel ni mixeur Windows.
  - *Le flux.* À chaque bloc envoyé à la carte, l'application copie aussi le bloc dans un anneau en mémoire partagée (« PianoForgeAudioFeed », `pianoforge/audio_feed.hpp`) puis publie le nombre de trames écrites. Pour le fil audio, c'est une copie mémoire et une écriture atomique, sans appel système. Le format change sous un compteur de séquence, appareil arrêté. Un seul écrivain est accepté : un second PianoForge entrelacerait ses blocs.
  - *Le plugin.* Il lit l'anneau toutes les 2 ms (minuterie haute résolution) et donne le son à OBS horodaté sur l'horloge d'OBS. Le flux suit l'horloge de la carte : avec ses horodatages, OBS laisserait l'écart grandir jusqu'à 70 ms puis sauterait. Le plugin rééchantillonne donc le flux avec le même rééchantillonneur que la sortie miroir, devenu commun (`pianoforge/drift_resampler.hpp`). Sa boucle s'accroche vite, puis passe en 6 s à des gains calmes, sans à-coup. Environ 20 ms de son attendent dans le plugin.
  - *Construction.* Contre l'OBS installé : les en-têtes de libobs de la même version sont téléchargés et vérifiés par empreinte, et la bibliothèque d'import est tirée des exportations de son `obs.dll`. Le plugin est installé dans `%ProgramData%/obs-studio/plugins/pianoforge-obs`, sans droits administrateur.
  - *Test dans le vrai libobs 33.0.1, sans l'application OBS* (`pianoforge-obs-test`). Le plugin chargé comme OBS le charge reçoit pendant 40 s un sinus de 1 kHz à 44,1 kHz, sur une horloge en avance de 200 ppm. Ce que libobs transmet est continu (saut d'horodatage maximal 0,35 ms). La fréquence est juste sur l'horloge d'OBS : 1000,19 à 1000,21 Hz pour 1000,20 attendus. Aucune discontinuité ; résidu −63 dB sur 20 ms.

**Ensuite, les phases.** Elles reprennent celles de [R] et y intègrent l'application.

| Phase | Contenu | Critère de sortie |
|---|---|---|
| 0. Squelette | Dépôt, CMake et CI sur trois systèmes, cœur avec son API C, application JUCE, MIDI horodaté, **capture des prises et export SMF dès le départ**, CLI de rendu | Un son simple joué sans décrochage ; une prise re-rendue à l'identique |
| 1. Une note juste | Bancs modaux complexes, B, accord étiré, marteau, unissons couplés par la cuisson, forge v0 | Tolérances respectées sur Iowa et bitKlavier |
| 2. Table et attaque | Filtres parallèles, résidu d'attaque, knock, partiels fantômes jusqu'à A3 | ABX sur des notes isolées |
| 3. Le piano entier | 88 notes, étouffoirs continus, demi-pédale, sostenuto, una corda, sympathie, bruits, refrappe | Glissando pédale enfoncée sans décrochage ; budget CPU tenu |
| 4. L'application | Prises (liste, réécoute, A/B, re-rendu haute qualité), réglages du joueur et du technicien, calibration du clavier, presets | Utilisable au quotidien |
| 5. Calibration automatique | Modèle différentiable, profil complet à partir de bitKlavier | Test MUSHRA contre l'enregistrement |
| 6. Extensions | Plugin, CC88 et MIDI 2.0, second profil, boîtier embarqué | — |

[R] estime environ 10 mois pour les phases 0 à 4, pour une petite équipe expérimentée. Pour une seule personne, surtout à temps partiel, il faut compter nettement plus. Les spikes donneront une première mesure réelle du rythme.

---

## 9. Décisions du 4 novembre 2026

| Sujet | Décision | Conséquences |
|---|---|---|
| Finalité | Projet personnel, sous forme de POC avancé ; une ouverture en open source est possible plus tard | JUCE sous AGPLv3 reste compatible avec une publication, et le cœur garde une licence permissive. Brevets et marques ne bloquent rien en usage privé ; on choisira des noms neutres en cas de publication. |
| Langage | Le plus optimisé et le plus propre, sans préférence personnelle | **C++20** pour le cœur, Python pour la forge et le banc, JUCE 8 pour la coquille |
| Clavier | Korg B2 | Voir ci-dessous |
| Référence sonore | Ni autre bibliothèque ni piano réel : on passe par Noire, sous Kontakt 8, avec le banc | Noire devient la **cible de calibration du POC**, et plus seulement la référence de comparaison. Un profil publié devra être recalibré sur des données à licence compatible (bitKlavier, Salamander, Iowa). |
| Banc | Installation des dépendances autorisée | Environnement `.venv` créé ; banc écrit et testé (§6.2) |
| Langue du logiciel | **English first** | Code, commentaires, noms de fichiers, commandes, formats de données et interface sont en anglais. Seuls les documents d'analyse comme celui-ci restent en français. |

**Ce qu'implique le Korg B2** (fiche Korg) :

- **USB type B pour le MIDI et l'audio.** Le B2 peut donc servir de carte son et jouer PianoForge sur ses propres haut-parleurs. Aucun pilote ASIO n'est connu pour lui : ce sera WASAPI en mode exclusif, avec une latence à mesurer au spike A.
- **La pédale fournie est un simple interrupteur** : le CC64 est tout ou rien. La demi-pédale demande le pédalier optionnel PU-2, à trois pédales, qui se branche sur la même prise. Sans lui, la demi-pédale, le sostenuto et l'una corda ne se testent qu'avec des CC synthétiques, ce que fait le banc.
- **Mesuré avec `bench keyboard` le 4 novembre 2026** (406 messages, capture dans `corpus/keyboard/`) :
  - 88 touches (A0 à C8) sur le canal 1 ;
  - vélocité en 7 bits. Lors d'un second test joué le plus fort possible (349 frappes), elle va de 3 à **114 au maximum** (A4), une seule frappe dépassant 111. Les maxima par registre sont 109 dans le grave, 114 dans le médium et 101 dans l'aigu. Sans courbe de vélocité, les couches les plus fortes de Noire (120 et 127) sont donc presque inaccessibles depuis le B2 ;
  - **pas de vélocité de relâchement** (toujours 64) ;
  - ni CC88 ni aftertouch.
- **La pédale fournie est un interrupteur à paliers.** Le CC64 passe par 0, 38, 74 et 127 en 10 à 140 ms, et aucune position intermédiaire ne tient plus de 110 ms : le B2 adoucit la transition, mais on ne peut pas tenir une demi-pédale. Ça confirme la fiche Korg (demi-pédale avec le PU-2 seulement).
- **Conséquences pour PianoForge.** La courbe de vélocité devra être calibrée sur la plage réelle du B2. Le relâchement ne pourra pas dépendre de la vitesse de la touche. Avec cette pédale, les étouffoirs continus ne seront jouables qu'à travers ces quatre paliers.

**Compléments du même jour**

| Sujet | Décision | Conséquences |
|---|---|---|
| Sortie audio | On joue sur le B2 et on écoute sur le B2 : PianoForge lui renvoie le son en USB audio | Le spike A mesure la latence par l'USB audio du B2 (WASAPI exclusif sous Windows, ALSA sous Linux). Il faudra couper le son interne du B2 pendant le jeu (fonction « Local Off », si le B2 la propose : à vérifier dans son manuel), sinon on entend les deux pianos à la fois. |
| Systèmes | Windows 11 en principal, mais **compatible Linux** | La CI compile et teste sous Linux à chaque commit. JUCE gère ALSA et JACK, et RTSan fonctionne sous Linux. Seul le banc reste propre à Windows, puisque Kontakt n'existe pas sous Linux ; le corpus, généré une fois, sert partout. Le B2 est probablement reconnu sans pilote sous Linux (classe USB standard), ce qui reste à vérifier. |
| Pédalier PU-2 | À décider plus tard | Rien ne presse pour le POC : le banc teste la demi-pédale avec des CC synthétiques. Il deviendra utile en phase 3, pour **jouer** la demi-pédale, le sostenuto et l'una corda. |

---

## Sources ajoutées par ce document

En plus de celles de [R] et [C] :

- ASIO sous double licence propriétaire et GPLv3 depuis octobre 2025 : [heise](https://heise.de/-10963451), [LibreArts](https://librearts.org/2025/11/steinberg-relicenses-vst3-and-asio/)
- Offres de licence JUCE 8 (Starter jusqu'à 20 000 $, Indie jusqu'à 300 000 $) : [forum JUCE, EULA JUCE 8](https://forum.juce.com/t/amendments-to-the-juce-end-user-licence-agreement-for-juce-8/61265), [forum JUCE, plafonds de revenus](https://forum.juce.com/t/revenue-limits-for-juce-tiers/61058/23)
- nih-plug en mode maintenance, avec un fork communautaire : [GitHub nih-plug](https://github.com/robbert-vdh/nih-plug) ; clack : [relatedrepos](https://relatedrepos.com/gh/prokopyl/clack)
- RealtimeSanitizer (Clang 20, pas de support Windows) : [GitHub rtsan](https://github.com/realtime-sanitizer/rtsan), [documentation LLVM](https://llvm.googlesource.com/llvm-project/+/b5132b7d044a5bc83eba9b09bd158cd77a511403/clang/docs/RealtimeSanitizer.rst)
- Hôtes de plugins pour le banc : [pedalboard](https://github.com/spotify/pedalboard), [DawDreamer](https://github.com/DBraun/DawDreamer) ; MIDI virtuel : [loopMIDI](https://www.tobias-erichsen.de/software/loopmidi.html), [Windows MIDI Services](https://github.com/microsoft/MIDI)
- Calculs de précision et de budget : `studies/resonator_precision.py`, `studies/resonator_budget.py` (Python standard, sans dépendance)
