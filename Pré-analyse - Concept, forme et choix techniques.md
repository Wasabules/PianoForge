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

**Phase 0 entamée.** L'application JUCE existe sous forme d'atelier d'écoute (`app/`) : clavier de 88 touches jouable à la souris, au clavier du PC (AZERTY ou QWERTY) et au clavier MIDI ; pédale forte ; choix de la sortie audio. Elle joue le corpus de référence derrière l'interface `pf::Engine` (`engine/`), celle que le moteur modal implémentera, ce qui permettra l'A/B. Un autotest sans interface (`PianoForge --selftest`) contrôle la fidélité de lecture, le rééchantillonnage, le relâchement et la charge.

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
