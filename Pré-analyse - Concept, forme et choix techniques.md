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
  - *Propriétés.* L'état de la connexion (fréquence, bloc, latence visée et latence courante, dérive compensée, recalages, durée), un bouton pour le rafraîchir ; la latence, de 10 à 100 ms (20 par défaut), changée à chaud ; un bouton « Resynchronise ». Le test vérifie l'état, une latence passée à 50 ms (49,2 ms en attente ensuite) et la resynchronisation ; il tourne sur un flux à son nom (`PIANOFORGE_AUDIO_FEED`), à côté d'un PianoForge et d'un OBS en marche. L'installation fonctionne OBS ouvert : la DLL chargée est renommée de côté, la nouvelle se charge au prochain démarrage.

**Atelier réorganisé (6 novembre 2026).** L'écran unique était devenu illisible. La fenêtre principale a maintenant une barre de menus (File, Engine, Audio, Keyboard, Help ; Player depuis le lecteur MIDI) et une barre d'outils : moteur (Reference ou Modal), enregistrement avec sa durée, volume, vélocité, pédale, charge audio et niveau de sortie. Viennent ensuite un tableau de bord de quatre cartes (moteur, dernière note, audio, session), le clavier, qui prend la place restante, et une barre d'état (corpus, profil, entrées MIDI, messages). Les réglages ont leurs fenêtres : *Engine settings* (ce que modélise le moteur, niveau de la résonance sympathique, refrappe, accordage, option du lecteur de référence), *Audio, MIDI and streaming* (périphérique, sortie miroir, état du flux OBS), *Velocity curve*, *Performance*. Chaque panneau garde ses propres réglages, appliqués dès le démarrage.
  - *Charge audio.* L'hôte audio mesure chaque bloc : le temps du traitement rapporté à la durée du bloc (QueryPerformanceCounter, sans appel système). Trois zones : sous 50 %, confortable ; de 50 à 80 %, chargé (un à-coup du système peut retarder un bloc) ; au-delà, coupures probables. La barre d'outils montre la moyenne, la crête des 2 dernières secondes et le nombre de coupures.
  - *Coupures*, comptées une seule fois chacune : blocs en retard (traitement plus long que le bloc), trous entre deux appels (pilote ou système), et celles que le pilote signale quand il le fait (l'ASIO oui, le mixeur de Windows non).
  - *La fenêtre Performance* donne les chiffres (instant, moyenne, crêtes sur 2 s et sur 1 min, millisecondes utilisées sur le budget du bloc), une minute d'historique et le détail des coupures. Elle donne aussi des conseils quand les réglages coûtent sans rien apporter : 192 kHz font travailler le moteur 4 fois plus qu'à 48 kHz pour rien d'audible (le corpus est à 44,1 kHz) ; des blocs de 10 ms ajoutent autant de latence ; l'ASIO donne la latence la plus basse.
  - `PianoForge --ui-snapshots <dossier>` enregistre une image de chaque panneau, pour vérifier une mise en page sans ouvrir de fenêtre.

**Lecteur de fichiers MIDI (6 novembre 2026).** Un fichier MIDI s'ouvre par File ou Player > Open MIDI file (Ctrl+O), en le déposant sur la fenêtre, ou en le passant à l'application (« Ouvrir avec »). Il se joue dans une barre sous le tableau de bord, qui donne :
  - lecture et pause (Ctrl+Espace), stop (Ctrl+Début) ;
  - la position, qu'on clique ou fait glisser (Ctrl+Gauche ou Ctrl+Droite : 5 s), avec le temps écoulé et le total ;
  - la vitesse, de 25 à 200 % ;
  - la boucle.

Le menu Player reprend ces commandes et ajoute des vitesses toutes faites.
  - *Lecture dans le fil audio, à l'échantillon.* Le fichier est lu sur le fil des messages, puis remis au fil audio par un pointeur publié. L'ancien fichier n'est libéré qu'une fois le fil audio sorti de son traitement : le fil des messages attend la fin du bloc en cours, qui dure quelques microsecondes. À chaque bloc, les événements qui y tombent sont ajoutés au MIDI du bloc, chacun à son échantillon le plus proche, avant le clavier à l'écran, qui montre donc les notes jouées. Ils ne passent pas par la file des messages : aucune gigue. Le moteur les reçoit comme ceux d'un clavier, sans courbe de vélocité (le fichier a ses propres vélocités). La capture des prises et l'enregistrement en direct les gardent.
  - *Déplacement, pause, stop.* Les notes que le fichier fait sonner sont relâchées, ainsi que sa pédale de sustain. À la reprise, les pédales sont remises dans l'état que le fichier leur donne à cet endroit. Une note tenue de part et d'autre d'un déplacement n'est pas refrappée.
  - *Ce qui est joué.* Les notes et les contrôleurs, dont les pédales, sur tous les canaux. Seule la batterie General MIDI (canal 10) est écartée quand d'autres canaux ont des notes, sinon elle sonnerait en notes de piano. Les temps SMPTE sont refusés.
  - *Autotest.* Un fichier est écrit sur disque, avec un changement de tempo et une note de batterie, puis relu et joué par l'hôte audio. Les six événements arrivent au moteur exactement à leur échantillon, à 100 % comme à 150 %. Au déplacement, à la pause et à la reprise, les relâchements et la pédale suivent la séquence attendue.

**Moteur : prises, sostenuto, étouffoirs, résonance du grave (6 novembre 2026).** Les quatre points retenus après le bilan du moteur.
  - *Rendu des prises.* « Save last take » rendait le WAV avec le seul choix de la table, du résidu et de la refrappe : l'accordage, la résonance sympathique, les touches tenues, le rattrapage à la pédale et le bruit de relâchement prenaient leurs valeurs par défaut. Le moteur donne maintenant tous ses réglages d'un bloc (`ModalEngine::settings()`, `applySettings()`), et le rendu les reprend tels qu'au moment de l'enregistrement. Un test du moteur vérifie qu'un second moteur ainsi réglé donne le même son, échantillon pour échantillon.
  - *Sostenuto (CC66).* À l'appui, il attrape chaque étouffoir levé à cet instant : ceux des touches tenues, et tous sous la pédale forte, comme la tringle d'un piano à queue. Ils restent levés jusqu'au relâchement, quoi que fassent les touches et la pédale forte ; un étouffoir levé ensuite n'est pas attrapé. Les cordes libérées résonnent par sympathie. Le lecteur de référence fait de même, pour garder l'A/B. Sans pédalier PU-2, la touche B tenue sert de sostenuto ; le lecteur MIDI le relâche au déplacement comme la pédale forte. Tests : moteur (touche tenue attrapée, touche frappée après, appui sous la pédale forte) et autotest du lecteur de référence (C4 attrapé tient 39 dB au-dessus du même jeu sans sostenuto, puis s'étouffe au relâchement).
  - *Étouffement par vélocité.* La courbe d'ensemble était une moyenne des trois vélocités mesurées (32, 80, 120). Or chez Noire les frappes fortes s'éteignent bien plus vite : après une tenue de 2 s, la courbe de v120 passe 7 dB sous la moyenne à 0,3 s et 11 dB à 0,6 s, celle de v32 reste 1 à 3 dB au-dessus (leur énergie est dans les partiels aigus, que le feutre arrête d'abord). `forge release-curves` mesure maintenant les courbes, d'ensemble et par groupe de partiels, à chaque vélocité ; le moteur joue celle de la vélocité de la note, interpolée en dB entre les vélocités mesurées (section `DAMV` du profil). La moyenne reste pour ce qui n'a pas de vélocité (l'étouffoir des cordes libres). Le lecteur de référence utilise les mêmes courbes : sa structure en double est remplacée par celle du moteur.
  - *Bruit de relâchement selon la tenue.* En cherchant pourquoi un relevé de pédale après une longue note restait trop fort, une seconde cause est apparue : chez Noire, le bruit de l'étouffoir faiblit avec la durée de la tenue (par rapport à une tenue de 2 s : +6 dB après 0,5 s, −3 à −12 dB après 10 s sur les notes étouffées, environ −10 dB par décade). Notre bruit avait le niveau des tenues courtes. `forge release-noise` mesure aussi ce niveau après chaque tenue : 0,5 et 2 s sur la passe des relâchements, 10 s sur les prises « pedal-up » de la séance interactions, identiques à la note de référence à v40 jusqu'au relâchement. Il obtient 0, −4,4 et −12 dB par rapport au niveau retenu ; le moteur interpole en log de la tenue (fin de la section `RELN`, ignorée par les anciens lecteurs). Le niveau d'un étouffement, après un relâchement de touche, ne dépend en revanche pas de la tenue au-delà de 2 s : passé 10 s, la note est si faible que le rapport mesuré n'est plus que ce bruit.
  - *Validation.* Niveau total après la pose de l'étouffoir, moteur contre Noire, en médiane des écarts sur les fenêtres 0,05–1,2 s :
    - relâchements de touche (132 prises, tenues de 0,5 et 2 s, v32, v80 et v120) : **2,3 → 0,8 dB** (90 % des fenêtres : 7,5 → 4,8 dB) ; par bandes d'octave, 2,9 → 1,4 dB ;
    - relevés de pédale après 10 s (30 prises de la séance interactions, v40 et v100, données que la calibration n'a pas vues) : **9,5 → 5,0 dB** (5,3 dB avec la résonance sympathique recalibrée plus bas, qui sonne dans ces prises). A3 à v100 suit maintenant Noire à 1 à 3 dB près sur la première seconde, là où il restait 7 à 15 dB trop fort (la « limite » notée plus haut venait du bruit, pas de la vélocité). Il reste un biais de −4 à −5 dB juste après le relevé. Par bandes d'octave, la médiane ne bouge pas (5,1 → 5,2 dB) : le bruit trop fort masquait, dans quelques bandes aiguës, un contenu que nous n'avons pas.
  - *Résonance sympathique du grave.* De A0 à D#3, elle sortait 6 à 9 dB trop faible sur les 0,2 premières secondes et 5 à 14 dB trop forte après 1,5 s. Un gain par corde n'y pouvait rien : c'est la forme dans le temps qui était fausse. Une note grave sonne plus de 10 s et entretient les cordes libres tout ce temps, alors que la résonance de Noire s'éteint d'elle-même.
    - *Première piste, écartée.* Une excitation qui décroît, exp(−t/τ), commune à toutes les notes. Ajustée sur une grille avec la montée, elle ne gagne presque rien (dispersion 7,53 contre 7,57 dB, τ = 4 s) : ce qui aide le grave dessert le reste.
    - *Retenu : une forme d'excitation (montée, τ) par registre de la note jouée.* Grave jusqu'à G#2 : montée de 0,05 s et τ = 4 s. Médium : 0,1 s, sans τ. Aigu à partir de A4 : 0,2 s, sans τ. Les gains par bande restent communs ; un gain par registre en plus n'apporte rien, les gains par bande l'absorbent. `forge sympathetic` essaie toutes les combinaisons de 25 formes sur les trois registres et garde celle dont le contrôle (modèle contre Noire, puissance totale par prise et par fenêtre) a le plus petit 90e centile d'écart. La dispersion des gains, utilisée jusqu'ici, suit mal ce contrôle. Le profil range les registres en fin de section `SYMP` ; un ancien lecteur les ignore.
    - *Contrôle de la forge* : la moitié des fenêtres à moins de 4,6 → 4,0 dB, 90 % à moins de 10,1 → 8,4 dB.
    - *Dans le moteur*, sur les 30 paires et cinq fenêtres : médiane +1,2 → +0,9 dB, moitié des fenêtres à moins de 4,7 → 3,8 dB, 90 % à moins de 11,0 → 8,7 dB. Dans le grave à v100, l'excès après 4 s tombe de +10 à +14 dB à +1 à +4 dB (A0, D#1, A1) ; sur 0–0,2 s, D#1 passe de −9,2 à −2,8 dB. Il reste un excès de 5 à 9 dB entre 0,5 et 4 s pour A0 à v100.

**Salle (6 novembre 2026).** Le moteur sortait un son sec : Noire est calibrée sans réverbération. `pf::Room` (`engine/`, sans JUCE, réutilisable par un plugin) ajoute une salle en sortie, quel que soit le moteur. Le son direct traverse sans être touché ni retardé ; seule la réverbération a son pré-délai.
  - *Premières réflexions.* Un modèle à sources-images d'une salle rectangulaire : les 24 images des ordres 1 et 2, du piano vers deux micros espacés de 0,6 m. Les dimensions suivent la taille, de 4,7 × 3,4 m à 21 × 15 m.
  - *Queue.* Un réseau de 16 lignes de retard rebouclées : longueurs en nombres premiers d'échantillons (29 à 113 ms pour la plus grande salle), mélange par une matrice de Hadamard sans perte, entrée diffusée par quatre passe-tout par canal.
    - *Absorption.* Par ligne, une perte qui fixe le temps de réverbération, et un plateau du premier ordre à 3 kHz qui raccourcit les aigus selon la brillance (filtres d'absorption de Jot). Le filtre passe-bas à un pôle habituel descend en pente douce de 0 à 24 kHz : mesuré, il ne raccourcissait presque pas les 2 à 8 kHz où se trouve la brillance du piano.
    - *Modulation.* Chaque longueur est modulée lentement, de 0,25 à 1 Hz et de ±0,125 ms : au plus 1,4 cent de hauteur, sans effet de chorus. La lecture fractionnaire passe par un passe-tout du premier ordre. Une interpolation linéaire dans la boucle étouffait les aigus à chaque passage, et le niveau sortait 4 dB trop bas.
  - *Niveaux.* L'énergie des premières réflexions est normalisée ; celle de la queue vaut 3 √(T60 / 2 s) fois la leur, d'après une estimation analytique (énergie stockée et décroissance), vérifiée à 0,7 dB près. Le réglage Level place ensuite la réverbération par rapport au son direct.
  - *Comportement.*
    - Le calcul se fait échantillon par échantillon : le résultat ne dépend pas de la taille des blocs.
    - Un changement de taille ou de pré-délai éteint la réverbération puis la rallume en 10 ms, sans clic, et la queue continue. Les autres réglages glissent sur 20 ms.
    - Coupée, la salle s'éteint, se vide, puis laisse passer l'entrée bit à bit.
    - En veille, une fois la queue sous −120 dB et l'entrée muette assez longtemps, elle ne coûte rien. Au réveil, elle repart d'un état propre.
  - *Dans l'atelier.* Un bouton Room dans la barre d'outils pour l'A/B, et la fenêtre Engine > Room settings (Ctrl+M). On y trouve cinq préréglages (Studio, Chamber, Concert hall, Large hall, Cathedral) et six réglages : niveau, taille, temps de réverbération, brillance, pré-délai, largeur. Par défaut, la salle est allumée sur Concert hall à −15 dB. Les enregistrements, le flux OBS et la sortie miroir l'incluent ; le rendu des prises (Ctrl+S) l'applique avec les réglages entendus et une queue allongée d'autant (mêmes réglages, phases de modulation reparties de zéro).
  - *Tests* (`room_test`, 16 contrôles, à 48 kHz) :
    - temps de réverbération mesuré (intégrale de Schroeder) : 2,01 s pour 2 s, 4,02 s pour 4 s, 1,95 à 1,97 s de 100 à 400 Hz ;
    - à brillance 0,3 : 1,00 s à 4–8 kHz et 0,89 s à 6–12 kHz, pour une cible de 0,81 s au-dessus de 3 kHz ;
    - densité d'échos normalisée de 1,03 dès 0,15 s, celle d'un bruit ;
    - dispersion du spectre de la queue de 5,8 dB autour de sa tendance (un bruit : 5,6 dB), donc pas de mode qui sonne ;
    - corrélation gauche/droite de −0,01 à pleine largeur, 1,000 à largeur nulle ;
    - 100 % de l'énergie d'un la à 1 kHz à moins de 4 Hz ;
    - pas de clic au changement de taille ;
    - contournement bit à bit quand la salle est coupée ;
    - même sortie en blocs de 64 ou de 509 ;
    - réglages extrêmes de 44,1 à 192 kHz finis ;
    - 60 s de silence en veille calculées en 4 ms ;
    - coût de 1,6 % d'un cœur.

    L'autotest de l'app vérifie qu'une note jouée par l'hôte audio garde son son direct (−0,2 dB d'écart) et gagne une queue de 8 dB après le relâchement. Au repos à 192 kHz, la charge reste à 0 %.

**Version navigateur, WebAssembly (6 novembre 2026).** JUCE 9 ne compile vers WebAssembly que `juce_core` : ni interface ni audio. L'atelier ne peut donc pas être porté tel quel. En revanche, le moteur modal et la salle, en C++ sans JUCE, tournent dans le navigateur (`web/`).
  - *Le module.* `web/src/exports.cpp` expose une interface C (`pf_*`), compilée par Emscripten 6.0.13 (emsdk dans `~/emsdk`, comme pour RevForge) en un module WebAssembly autonome de 130 Ko. Il n'a pas de JavaScript Emscripten ; sa seule importation est la notification de croissance de la mémoire. L'AudioWorklet l'instancie sur le fil audio du navigateur, par blocs de 128 échantillons, comme dans RevForge.
  - *Le moteur rendu portable.*
    - L'analyse du profil se fait depuis la mémoire (`parseModalProfile`), et la lecture du fichier est rangée à part : le module web n'a pas de système de fichiers.
    - L'installation d'un profil n'attend plus le fil audio dans le navigateur, puisqu'elle s'y fait.
    - Le flux OBS reste propre à l'application native.
    - Les tests du moteur tournent sous Node (`ctest --test-dir build/wasm`) et passent tous, y compris les mesures acoustiques de la salle.
  - *Trois corrections, venues des mesures.* Le premier module coûtait 130 % du temps réel pour 64 voix pédale enfoncée, contre 17 à 20 % en natif.
    - *Les dénormaux* : WebAssembly ne permet pas le « flush-to-zero ». Les partiels éteints (sous 1e-38) restaient dans les voix, et le coût montait de 18 à 270 % en 8 s, alors même que les voix diminuaient. Les phaseurs morts (|u| < 1e-12, −240 dB) sont maintenant remis à zéro à chaque bloc, en natif aussi, sans effet audible. Les états de filtres de la salle qui s'éteignent subissent le même traitement.
    - *Les chemins SIMD WebAssembly explicites* (128 bits, sans FMA) pour les bancs de résonateurs et les cordes libres : le chemin générique sur 8 voies se vectorisait mal, surtout pour les cordes.
    - *La mémoire initiale de 320 Mo* : les agrandissements successifs coûtaient 2,5 s à l'analyse du profil, contre 0,1 s maintenant.
  - *Résultats* (`web/check.mjs`, Node 22) :
    - le profil s'analyse en 0,1 s ;
    - le son est celui du moteur natif à 0,03 dB près par fenêtre. Les échantillons s'écartent de −50 dB sur les notes longues, par une dérive de phase due aux arrondis sans FMA ;
    - 64 voix pédale enfoncée avec les cordes libres coûtent 55 à 60 % du fil audio à 48 kHz, salle comprise, soit environ 3 fois le natif, et ce coût reste stable dans le temps.

    Pour ne pas calculer pour un périphérique à 192 kHz, la page demande un contexte audio à 44,1 kHz, la fréquence du profil ; le navigateur convertit lui-même vers la sortie.
  - *La page* (`web/site/`, sans outil de construction) reprend les couleurs de l'atelier :
    - un clavier de 88 touches, jouable à la souris (plus bas sur la touche, plus fort) et au clavier de l'ordinateur ; les touches sont repérées par leur position physique, donc AZERTY et QWERTY marchent sans réglage ;
    - la pédale forte (Espace) et le sostenuto (B) ;
    - Web MIDI, avec la vélocité maximale du clavier MIDI réglable ;
    - le volume, la salle (préréglages de l'atelier et niveau), les options du moteur, la résonance sympathique et le La.
  - *Le profil ne quitte jamais le navigateur.* On choisit `modal-profile.bin` une fois ; la page le garde dans IndexedDB et le transmet au fil audio. Calibré sur Noire, il ne peut pas être publié : la page ne pourra être mise en ligne qu'avec un profil tiré d'une source libre.
  - *Essai dans un vrai navigateur* (Edge sans fenêtre, profil temporaire, son coupé) : profil chargé dans le fil audio (88 × 16), moteur à 44,1 kHz, latence de sortie annoncée d'environ 10 ms. Un C4 joué au clavier de l'ordinateur avec la pédale donne 1 voix, la couche 96, le voyant de pédale et la touche enfoncée. Aucune erreur dans la console. La latence réelle dépend du système et du navigateur (10 à 40 ms), loin de l'ASIO.

**Lecteur MIDI partagé, et le temps à la vitesse (6 novembre 2026).** Le lecteur de fichiers MIDI passe dans le moteur, sans JUCE (`pianoforge/midi_sequence.hpp`). Une seule implémentation sert l'atelier et la page web.
  - *Lecture des fichiers* (`readMidiFile`) : fichiers MIDI standard des formats 0, 1 et 2, toutes pistes, carte des tempos, statut courant ; les messages système exclusifs et méta sont sautés ; la batterie GM est écartée quand d'autres canaux jouent ; les temps SMPTE et les fichiers sans note sont refusés ; toutes les lectures sont bornées (un fichier tronqué ne déborde pas). L'atelier l'utilise à la place de `juce::MidiFile`. Son autotest relit à l'identique le fichier écrit par JUCE.
  - *Lecture à l'échantillon près* (`SequencePlayer`) : celle de l'atelier, inchangée (déplacement, pause, vitesse, boucle, relâchements, rétablissement de la pédale), produisant des `pf::Event`. L'atelier les verse dans le bloc MIDI de l'hôte audio ; le module web, dans le moteur, dans le fil audio du navigateur.
  - *Test* (`sequence_test`, natif et sous Node) : un fichier construit en mémoire, avec deux pistes, un changement de tempo, du statut courant, un message système exclusif, un changement de programme et une note de batterie. Les temps sont exacts, chaque événement tombe sur son échantillon à 100 % et à 150 %, et le déplacement, la pause, la reprise et la boucle suivent la séquence attendue. Les fichiers SMPTE, ceux qui ne sont pas des fichiers MIDI et les fichiers tronqués sont refusés.
  - *Dans la page web* : une barre de lecteur comme celle de l'atelier. On ouvre un fichier par le bouton ou en le déposant sur la page, même avant le démarrage. On y trouve lecture/pause (Ctrl+Espace), stop (Ctrl+Début), une barre de position avec ±5 s (Ctrl+Gauche ou Droite), la vitesse de 25 à 200 % et la boucle. Les touches jouées par le fichier s'allument sur le clavier. Essai dans Edge sans fenêtre : la démo à 150 % fait sonner 10 voix et allume les touches du fichier.
  - *Le temps à la vitesse, dans les deux.* Le compteur principal avance d'une seconde par seconde réelle, sur la durée à cette vitesse (durée du fichier ÷ vitesse). Hors de 100 %, le temps du fichier suit entre parenthèses, par exemple `0:03 / 0:15   (file 0:04 / 0:23)` à 150 %. Un changement de vitesse recalcule la durée restante.

**Parties d'un fichier MIDI et transposition (6 novembre 2026).** Beaucoup de fichiers MIDI portent plusieurs instruments ; d'autres sont écrits une octave trop bas, ou dans un autre ton.
  - *Parties.* Une partie est le couple (piste, canal) qui porte des notes, avec le nom de sa piste, son instrument General MIDI et son nombre de notes. Toutes sont jouées, sauf la batterie (canal 10) quand d'autres jouent. Une partie coupée relâche ses notes, et les pédales de son canal si aucune autre partie de ce canal ne joue. Les contrôleurs d'une piste sans note vont à la première partie de leur canal. Dans l'atelier, le bouton « Tracks n/m… » ouvre la liste avec All et None ; dans la page web, une liste déroulante.
  - *Transposition*, de −48 à +48 demi-tons, par demi-ton ou par octave. Un changement relâche ce qui sonne, puis rétablit les pédales. Les notes transposées hors de la plage MIDI (0 à 127) sont écartées.
  - *Tests* : `sequence_test` (la batterie, coupée par défaut puis allumée, joue sa note ; le piano coupé en cours de note relâche ses notes et ne joue plus ; une octave plus haut, puis un retour à 0 en cours de lecture qui relâche les notes transposées ; la borne de ±48) et l'autotest 8d de l'atelier (3 notes, 7 événements, 2 parties).

**Fréquence interne du moteur (6 novembre 2026).** En mode Windows Audio, le mélangeur de Windows impose sa fréquence au périphérique, jusqu'à 192 kHz sur le FiiO. Le moteur faisait alors quatre fois le travail de 48 kHz pour rien d'audible (le corpus est à 44,1 kHz).
  - *Principe.* Au-delà de 50 kHz, le moteur et la salle tournent à la fréquence du périphérique divisée par 2, 4 ou 8 (192 → 48 kHz, 176,4 → 44,1 kHz, 96 → 48 kHz). Un suréchantillonneur polyphase ramène le son à la fréquence du périphérique (`pf::Upsampler`, sinus cardinal fenêtré par Kaiser, 64 coefficients par phase, β = 9). Il est plat jusqu'à 20 kHz, ses images sont à plus de 90 dB sous le signal, et il ajoute 0,7 ms de latence. Son coût est de 0,9 % d'un cœur par canal à ×4.
  - *Hôte audio.* Les blocs du périphérique ne tombent pas toujours sur un multiple du facteur (1001 échantillons à ×4, par exemple). Le moteur rend donc les trames qui couvrent le bloc, et les échantillons de trop attendent le bloc suivant. Les événements MIDI tombent sur la trame qui contient leur position. Le lecteur de fichiers place les siens aux positions du périphérique des trames du moteur. Enregistrements, prises, sortie miroir et flux OBS restent à la fréquence du moteur.
  - *Réglage*, dans Audio > Engine rate : automatique (recommandé) ou à la fréquence du périphérique. Un changement redémarre le périphérique. Le tableau de bord affiche les deux fréquences, et le conseil « 192 kHz » ne s'affiche plus quand le moteur tourne plus bas.
  - *Tests.* `upsampler_test`, natif et sous Node : gain à 0,05 dB près à 1 et 19 kHz, images à −96,5 dB (×2) et −100,3 dB (×4), latence de la moitié du filtre, décimation sans repliement (−111 à −115 dB). Autotest 8f : avec un périphérique à 176,4 kHz et des blocs de 1001, le moteur tourne à 44,1 kHz (×4). Le niveau est celui d'un rendu à 44,1 kHz à 0,001 dB près, fenêtre par fenêtre. Au-dessus de 24 kHz, on mesure −115,6 dB : aucune couture entre les blocs. En mode « fréquence du périphérique », le moteur tourne à 176,4 kHz. L'autotest 8 (prise re-rendue à l'identique) reste exact à 44,1 kHz.

**Formats d'enregistrement (6 novembre 2026).** L'enregistrement en direct (Ctrl+R) reste un WAV 32 bits flottant, écrit pendant le jeu. Sur le fil audio, il n'y a toujours qu'une copie dans une file sans verrou ; un fil d'écriture vide cette file sur le disque. Les autres formats sont fabriqués à partir de ce WAV quand l'enregistrement s'arrête, sur un fil de fond.
  - *Pourquoi à la fin plutôt qu'en temps réel.* Aucun encodeur ne tourne pendant le jeu, donc rien ne peut coûter un décrochage. Si une conversion échoue, le WAV 32 bits reste là : rien n'est perdu. Et c'est rapide : 3 s de son se convertissent en 20 à 120 ms selon le format, soit une heure de jeu en moins de deux minutes pour le MP3, plus lent que les autres.
  - *Formats*, à cocher dans File > Record formats :
    - WAV 32 bits flottant (le son tel que joué) ;
    - WAV 24 bits ;
    - FLAC 24 bits (sans perte, les mêmes échantillons que le WAV 24, environ le tiers de la taille) ;
    - MP3 320 kbit/s, par l'encodeur de Media Foundation, livré avec Windows 8 et suivants (absent des éditions « N » sans le Media Feature Pack, et de Linux) ;
    - Opus 192 kbit/s (JUCE 9, sur tous les systèmes), plus fidèle que Vorbis au même débit.

    Un seul WAV à la fois, puisque les deux porteraient le même nom. Le MIDI reste à part, aligné à l'échantillon sur les fichiers sans perte. Les fichiers compressés commencent après le délai de leur encodeur (quelques dizaines de millisecondes pour le MP3).
  - *Détails.* Le MP3 ne prend que 32, 44,1 ou 48 kHz : au-dessus, le son est d'abord ramené à 44,1 ou 48 kHz par `pf::Downsampler`. Il reçoit du 16 bits, avec un tramage triangulaire. Opus travaille à 48 kHz et rééchantillonne lui-même. Le WAV 24 bits garde les métadonnées Broadcast WAV du WAV 32 bits.
  - *Fichiers.* Si le WAV 32 bits n'est pas coché, l'enregistrement s'écrit dans `<nom>.master.wav`, effacé une fois toutes les conversions réussies. Si une conversion échoue, ou si l'on quitte pendant une conversion, il est renommé `<nom>.wav`. La carte Session affiche la conversion en cours (« Converting FLAC 40 % »), et la barre d'état la liste des fichiers produits.
  - *Autotest 8g.* Un WAV 32 bits de 3 s (1 kHz à −6 dBFS à gauche, 440 Hz à −12 dBFS à droite) est converti dans chaque format, puis relu :
    - WAV 24 et FLAC : écart de −141 dBFS, sous le demi-pas de 24 bits, et le FLAC a exactement les échantillons du WAV 24 ;
    - MP3 : 3,048 s, niveaux à 0,01 dB près, 326 kbit/s mesurés ;
    - Opus : 3,000 s, niveaux à 0,01 dB près, 198 kbit/s ;
    - un WAV à 96 kHz donne un MP3 à 48 kHz au même niveau ;
    - un travail de fond complet (FLAC et Opus) se termine en 0,1 s et efface le master.

**Distance d'écoute, sur la physique (6 novembre 2026).** Jusqu'ici, on ne savait pas d'où l'on écoutait : la salle ajoutait une réverbération à un niveau choisi à l'oreille. Désormais, une position d'écoute en mètres règle tout, par la physique.
  - *Convention.* Le son du moteur (Noire, micros proches) est le piano entendu du clavier, à 1 m. À la position par défaut, « At the keyboard (1 m) », le son direct est celui du moteur, bit pour bit.
  - *La salle* est une boîte de proportions 10,5 : 7,5 : 4,5, dont le volume suit la taille, de 60 à 40 000 m³ en progression géométrique. Les préréglages ont de vrais volumes :
    - Studio, 150 m³, 0,6 s ;
    - Chamber, 1 600 m³, 1,4 s ;
    - Concert hall, 15 000 m³ (37 × 26 × 15,7 m), 2,2 s ;
    - Large hall, 25 000 m³, 3 s ;
    - Cathedral, 40 000 m³, 5,5 s.

    Le piano est près d'une extrémité. L'auditeur se place dans l'axe, à la distance demandée, sans sortir de la salle : dans le studio, environ 5,5 m au plus.
  - *Énergies* : théorie révisée de Barron (le champ diffus de Sabine, diminué de la décroissance déjà écoulée quand le son direct arrive), pour une directivité Q = 2. On a A = 0,161 V/T. Les réflexions valent 16π r₀²/(Q A) × e^(−13,82 r/(c T)), à répartir entre les 80 premières ms (précoces) et la suite (tardives). Le direct vaut (r₀/r)².
    - Dans la grande salle, la distance critique est de 6,6 m. Au clavier, la salle est 16,5 dB sous le direct ; au fond (20 m), elle le dépasse de 8 dB environ.
    - Le réglage Level devient une correction autour de cette valeur (0 dB : la physique).
    - Les réglages enregistrés de l'ancien modèle (un niveau contre le son sec) sont remplacés par leur préréglage, au clavier.
  - *Réflexions précoces.* Images du premier et du second ordre, sauf celles du sol : le son du moteur a déjà la sienne. Elles vont vers deux micros espacés de 0,6 m à la position d'écoute.
    - Chaque mur garde √(1 − ᾱ) de l'amplitude, avec ᾱ = A/S, l'absorption moyenne de Sabine pour ce temps de réverbération dans cette salle.
    - Le pré-délai vient de la géométrie (la première réflexion, 32 ms au clavier dans la grande salle) ; le réglage Pre-delay disparaît.
    - Le gain des réflexions et celui du réseau sont résolus ensemble (un système 2 × 2) pour respecter les deux fenêtres de Barron. Dans une grande salle, des réflexions du second ordre arrivent après 80 ms ; dans une petite, le réseau commence avant.
  - *Son direct* plus loin :
    - il décroît en 1/r ;
    - l'air l'absorbe (ISO 9613-1, 20 °C, 50 % d'humidité, 4,66 dB/km à 1 kHz), par deux passe-bas du premier ordre ajustés à 10 kHz ;
    - il se rétrécit selon l'angle sous lequel on voit le piano (1,5 m de large) : 0,145 de la largeur à 8 m.

    Un piano lointain est une source ponctuelle : chaque oreille en reçoit toute la puissance. Le rétrécissement et la somme mono des réflexions gardent donc l'énergie d'une entrée stéréo peu corrélée. Le gain se calcule sur les énergies milieu/côtés de l'entrée (200 ms), 1 pour une entrée mono, √2 au plus. Sans cela, le son direct d'un accord perdait 2,8 dB au premier rang.
  - *Keep loudness* (activé par défaut) : tout est mis à l'échelle pour garder l'énergie totale entendue au clavier. S'éloigner change l'équilibre, pas le volume.
  - *Ce qui a été corrigé par la mesure.* L'énergie du réseau est estimée avec son T60 à 1 kHz (l'étagère des aigus le raccourcit déjà là). Mesuré, le réseau donne 0,7 dB de moins que l'estimation : l'énergie n'est pas répartie uniformément dans les lignes. La correction est appliquée et documentée dans le code.
  - *Mesures* (`room_test`, natif et sous Node), impulsions, bande 250 Hz–2 kHz, énergies par Parseval :
    - réflexions contre direct : à ±1,2 dB de Barron dans les quatre salles, entre 2 et 16 m (grande salle : −10,7 dB à 2 m pour −10,6 en théorie, +6,0 dB à 16 m pour +6,4) ;
    - précoces et tardives : à ±1,2 dB, sauf le tardif du studio (T60 0,6 s), à −2 dB ;
    - direct exactement en 1/r ;
    - keep loudness : −0,38 dB à 16 m contre le clavier ;
    - air à 20 m : à 0,1 dB de l'ISO jusqu'à 10 kHz. À 16 kHz, 5,3 dB au lieu de 6,9 : des filtres du premier ordre ne suivent pas la loi en f² près de Nyquist, ce qui reste inaudible sur un piano ;
    - largeur : l'angle exact ;
    - aucun clic quand la distance change en cours de jeu ;
    - coût de 2,0 à 2,5 % d'un cœur.
  - *Autotest 8e*, un accord dans la grande salle :
    - au clavier, la salle s'ajoute seulement : 0,0 dB pendant l'accord ;
    - au premier rang avec keep loudness, l'accord a le même volume sur ses 3 s (−0,7 dB), et la salle domine sa queue (+7,9 dB).
  - *Dans l'atelier* : la fenêtre Room a deux cartes, la salle et la position d'écoute (cinq positions : au clavier 1 m, sur scène 3 m, premier rang 6 m, milieu de salle 12 m, fond 20 m ; la distance de 1 à 30 m ; keep loudness). Un résumé donne ce que la physique donne là : volume et dimensions, distance critique, salle contre direct, première réflexion, niveau, air et largeur du direct.
  - *Dans la page web* : une liste des positions à côté du préréglage, keep loudness toujours actif.

**Cordes couplées, una corda et accord de l'unisson (6 novembre 2026).** Les 2 ou 3 cordes d'une note sont accordées presque pareil et tirent toutes sur le chevalet, qui bouge et les couple.
  - *Ce qu'on avait.* Les modes mesurés sur Noire contenaient déjà les battements et la double décroissance (son immédiat, puis son rémanent), mais figés : chaque frappe battait pareil, sans una corda ni réglage d'unisson.
  - *Noire et l'una corda.* Des prises avec et sans CC67 (do4 v40 et v100, la1 v100, do6 v100) sont identiques au bit près : Noire ignore la pédale douce. L'una corda ne peut donc être que physique, sans référence.
  - *Modèle* (Weinreich, 1977), par partiel et pour une polarisation. On a A = diag(s_i) + Y·J :
    - s_i = −σ + 2πi·f_i, les pôles propres des cordes (amortissement commun, fréquences désaccordées) ;
    - Y, l'admittance du chevalet vue par les cordes ;
    - J, la matrice de uns.

    Le marteau frappe avec des poids h, et le chevalet somme les cordes. A est complexe symétrique, d'où des résidus simples : r_k = (Σ v)(Σ v·h)/(Σ v²), avec v_i = 1/(λ_k − s_i).
    - Frappées pareil, les cordes bougent ensemble : c'est la décroissance rapide.
    - Leur désaccord fuit vers les mouvements où elles s'opposent. Ceux-ci remuent peu le chevalet, meurent lentement (le son rémanent) et battent contre le premier.
    - Cordage d'un Steinway D : 10 notes à une corde, 18 à deux, 60 à trois.
  - *Ajustement* (`forge/unison.py`, dans l'export : section `UNIS`). Les pôles des N modes mesurés les plus énergétiques du partiel sont les valeurs propres. Pour un Y donné, les s_i en découlent : le polynôme caractéristique de A vaut p − Y·p′, donc p = q + Y·q′ + Y²·q″ + ….
    - À trois cordes, on choisit Y pour que les cordes aient le même amortissement.
    - À deux cordes, il manque une condition. On garde le chevalet le plus résistif possible, avec un amortissement propre positif.
    - Résultat sur tout le profil : 16 643 des 23 021 partiels des notes à 2 ou 3 cordes (87 % des bicordes, 52 % des trichordes).
    - Les trichordes résolus donnent, en médiane, un amortissement propre de 1,2 /s, un chevalet de 1,8 /s et un étalement de l'unisson de 1,3 cent (de 0,5 à 5,8). C'est un unisson bien accordé.
  - *Dans le moteur*, à la frappe, seulement si l'una corda ou le réglage d'unisson s'écarte du défaut :
    - le modèle de chaque partiel est résolu (valeurs propres d'une matrice 2 × 2 ou 3 × 3, par Durand-Kerner), puis ajusté aux modes mesurés (α, par moindres carrés) ;
    - ce que le modèle n'explique pas garde son amortissement mesuré et ne suit que son changement de fréquence (c'est le battement) ;
    - la part du modèle devient α·r, sur le pôle déplacé ;
    - l'énergie du partiel est bornée à +6 dB.

    Par défaut, le son est exactement celui d'avant (même au bit près). Deux premières formes ont été abandonnées sur mesure :
    - multiplier chaque mode par r′/r donnait jusqu'à +14 dB quand le modèle sous-estime un mode fort ;
    - déplacer aussi l'amortissement de la part non expliquée faisait remonter la fin de note.
  - *Una corda* (CC67, continu ; touche N au clavier de l'ordinateur ; voyant Soft). Le marteau décalé manque une corde de chaque unisson (h = 1 − pédale sur la corde accordée le plus haut ; on ne sait pas laquelle, l'effet en dépend peu). Il frappe aussi avec un feutre plus tendre, estimé à −1,5 dB par octave de numéro de partiel. Sans modèle, la part frappée est (N − pédale)/N.
    - Rendus sur le vrai profil : −1,4 à −3,9 dB à l'attaque ;
    - le son rémanent est relativement plus présent (+1 à +4 dB sur 2–5 s dans le grave et le médium) ;
    - le timbre est plus doux (centroïde −16 à −69 Hz).
  - *Accord de l'unisson* (Engine settings, « Unison tuning », 0 à 300 %, 100 % : comme mesuré). Les désaccords des cordes sont multipliés.
    - À 0 % : plus de battement. L'enveloppe du do4 v40 s'écarte de 0,08 dB d'une décroissance unique, contre 1,70 dB mesuré ; v100 : 0,12 contre 2,02.
    - À 200–300 % : un piano droit fatigué, qui bat plus et garde plus de son rémanent.
    - Le réglage s'applique aux notes frappées ensuite.
  - *Tests.* Forge (`test_unison`) : un système connu (deux et trois cordes) est retrouvé, et l'una corda affaiblit le mode rapide en excitant le lent. Moteur (test 14, sur un trichorde synthétique) :
    - par défaut, rien ne change au bit près ;
    - una corda : attaque −3,5 dB, son rémanent +6,9 dB relativement à l'attaque ;
    - unisson parfait : une seule décroissance (0,15 dB d'écart, contre 5,8) ;
    - désaccord ×3 : battement plus profond (7,4 dB).
  - *Limites.*
    - Une seule polarisation, avec les N modes les plus forts. Les autres modes du partiel restent tels que mesurés, à l'una corda près, qui ne leur applique que le feutre.
    - Un trichorde sur deux n'a pas de modèle (pas de chevalet physique qui reproduise ses trois pôles) : il ne reçoit que la part frappée.

**L'extrême aigu, revu (6 novembre 2026).** Mesuré à nouveau, par bandes et par fenêtres, sur C6, C7, E7, A7 et C8 de v40 à v112.
  - Depuis que le résidu d'attaque se prolonge (jusqu'à 0,5 s), le moteur suit Noire à ±1 dB au total sur toute la première seconde. Les modes du fondamental, refits sur des fenêtres plus longues, reproduisent la prise à 1–2 dB jusqu'à 2 s : ils n'étaient pas en cause.
  - Ce qui manquait, entre 50 et 60 dB sous la crête de la note : entre 1 et 3 s, l'énergie de Noire est surtout sous 500 Hz (la table d'harmonie qui sonne encore, −82 à −124 dBFS). La table du moteur s'éteignait 2 à 9 dB trop tôt, puis était coupée sous −100 dB.
  - Correctif : la table s'éteint désormais sous −120 dB. Entre 2 et 3 s, la bande 60–120 Hz est à −1 à +9 dB de Noire, au lieu du silence.
  - Reste, inaudible : la bande 500–2000 Hz des notes les plus aiguës après 0,5 s (−5 à −25 dB, à −88 à −96 dBFS), entre la table (≤ 700 Hz) et le fondamental.

**Pédale, préréglages de salle et périphérique audio (7 novembre 2026).**
  - *Courbe de pédale* (Keyboard > Pedal curve). Comme la courbe de vélocité, elle porte sur la plage que la pédale du clavier MIDI couvre vraiment (certaines n'atteignent jamais 0 ni 127, d'autres n'envoient que quelques paliers) et sur cinq points, avec une courbe monotone. Un apprentissage de la plage est proposé : enfoncer la pédale lentement jusqu'au bout, la relâcher. La fenêtre montre la bande où les étouffoirs ne font qu'effleurer les cordes, tirée de la courbe mesurée du profil : on y place la demi-pédale dans la course de sa pédale. La courbe ne s'applique qu'à la pédale du clavier MIDI, ni à la barre d'espace ni aux fichiers MIDI.
  - *Le bouton Pedal* se remplit selon la course reçue par le moteur (CC64, quelle qu'en soit la source), avec le pourcentage en demi-pédale.
  - *Préréglages de salle* : la salle et la position d'écoute s'enregistrent sous un nom (fenêtre Room : Save…, un nom existant est remplacé ; Delete, après confirmation). Ils apparaissent sous « My presets » dans la liste, et dans Engine > Room presets pour changer vite.
  - *Le périphérique audio choisi n'est plus perdu.* Deux causes, corrigées :
    - en changeant de type dans la fenêtre Audio, JUCE ouvrait le premier pilote du type (« ASIO Link Pro ») et non le dernier utilisé. L'application retient maintenant le dernier pilote de chaque type ;
    - un périphérique qui ne s'ouvre pas au démarrage (pilote encore tenu par une application qui se ferme, ou par un autre programme) était remplacé, puis le remplaçant était enregistré à la place du choix. Le choix est maintenant gardé, l'application le réessaie pendant 15 s et l'annonce dans la barre d'état. Le pilote ASIO du FiiO accepte deux clients, donc ce second cas ne le touchait pas.

**Limiteur de sortie, et la page web à niveau (7 novembre 2026).**
  - *Limiteur* (`pf::Limiter`, sans JUCE, après la salle : dans l'atelier, le rendu des prises et la page web). Rien ne dépasse −1 dBFS sur la carte son, dans un enregistrement ou sur le flux OBS. Un gros accord avec la salle pouvait le faire.
    - Sans anticipation, donc sans latence ajoutée, puisqu'on joue en direct. L'échantillon qui dépasserait est ramené exactement au plafond, le gain est tenu 5 ms puis remonte avec une constante de 100 ms, sans jamais laisser passer un échantillon suivant. Le même gain sur les deux canaux, pour que l'image ne bouge pas.
    - Tant qu'il n'agit pas, le son passe au bit près ; il revient au bit près dès que le gain est à 0,004 dB de l'unité.
    - Les crêtes entre échantillons ne sont pas recherchées : le plafond à −1 dBFS leur laisse de la marge.
    - Réglage et compte rendu dans Audio > Output (« Never needed so far » ou la réduction la plus forte), activé par défaut.
    - *Tests* (`limiter_test`, natif et sous Node) : transparent sous le plafond ; une rafale à +6 dBFS reste sous −1 dBFS (−7 dB de réduction) ; même gain sur les deux canaux ; retour exact à l'unité une seconde après ; même résultat pour des blocs de 64 ou 509 échantillons ; éteint, le son passe intact.
  - *Page web à niveau.* Les réglages sont dans une fenêtre (bouton ⚙ Settings à droite de la barre, fermée par Échap, la croix ou un clic à côté), à quatre onglets : Sound, Room, Keyboard & pedal, Tuning. Chaque réglage y a son explication ; la barre ne garde que l'accès rapide à la salle et à la position. Le clavier de l'ordinateur ne joue pas tant que la fenêtre est ouverte. Les onglets :
    - « Room and listening position » : la distance au mètre, liée à la liste des positions (Custom hors préréglage), Keep loudness, et ses propres préréglages (Save…, Delete), sous « My presets » dans la liste des salles ;
    - « Sustain pedal » : la plage envoyée par la pédale du clavier MIDI, la forme de la courbe (loi de puissance, l'atelier en dessine cinq points), l'apprentissage de la plage et la valeur en direct.

    Le voyant Pedal se remplit selon la course reçue par le moteur (`pedalValue`, nouveau dans le moteur). Essai dans Edge sans fenêtre : un préréglage enregistré revient avec sa distance et son Keep loudness, puis se supprime ; la courbe est mémorisée.

**Préréglages de l'instrument, sauvegarde et restauration (7 novembre 2026).**
  - *Menu Presets.* « Save the instrument as… » garde toute l'installation sous un nom : le moteur et ses réglages (options, résonance, unisson, refrappe), l'accord, la salle et la position d'écoute (avec le préréglage de salle choisi), les courbes de vélocité et de pédale, le volume. Un nom existant est remplacé. Les préréglages se choisissent dans le menu, se suppriment dans Delete après confirmation, et le préréglage en cours est coché. Le périphérique audio, le corpus, les dossiers et les formats d'enregistrement n'en font pas partie.
  - *Export et import des réglages* (Presets > Export settings… / Import settings…) : un fichier `.pfsettings` (XML) qui contient tous les réglages, les préréglages de l'instrument et de salle et les courbes. Les réglages propres à l'ordinateur sont laissés de côté : périphérique audio, pilote par type, corpus, dossiers, sortie miroir, fréquence du moteur. L'import s'applique tout de suite, sans relancer.
  - *Mécanisme commun* : `EnginePanel::reload()` et `RoomPanel::reload()` relisent tout depuis le fichier de réglages et l'appliquent au moteur et à la salle. L'autotest 8h modifie un fichier sous les panneaux (unisson 2,5, table coupée, La 442 Hz, une salle perso « Hall, 12 m ») et vérifie le moteur et la salle après relecture.

**Le gestionnaire de prises (7 novembre 2026).**
  - *Une seule liste* (File > Takes and recordings…, Ctrl+T) pour les prises gardées par Ctrl+S (dossier Takes) et les enregistrements faits par Ctrl+R (dossier Recordings). Les fichiers d'un même nom sont regroupés (`.mid`, `.wav`, `.flac`, `.mp3`, `.opus`, le maître `.master.wav`). Chaque ligne donne le type, la date, la durée, le nombre de notes (lu dans le MIDI) et les fichiers présents. Le tri se fait par colonne, les plus récentes d'abord.
  - *Réécoute.* Listen (ou Espace) lit le meilleur fichier disponible : WAV, puis FLAC, puis Opus, puis MP3, puis le maître. Deux prises sélectionnées se comparent en A/B *au même instant* : les touches A et B passent de l'une à l'autre avec un fondu de 10 ms, sans revenir au début. Le curseur déplace la lecture ; un fichier à une autre fréquence que la carte est lu par interpolation linéaire, ce qui suffit pour une réécoute. Le moteur continue de jouer pendant la réécoute.
  - *Ce qu'on entend n'est ni enregistré ni diffusé.* La réécoute s'ajoute au son après les prélèvements de l'enregistrement et du flux OBS, et après le limiteur : une prise réécoutée pendant un enregistrement n'y entre pas. Le passage entre le fil de l'interface et le fil audio suit un protocole de Dekker (`ready` et `rendering`), si bien que le fil audio ne bloque jamais et ne lit jamais un tampon en cours de remplacement.
  - *Rejouer et re-rendre.* « Play with the engine » rejoue le MIDI de la prise par le lecteur de fichiers, donc avec le moteur et les réglages actuels. « Render again » en fait un nouveau WAV, « <nom> (render AAAAMMJJ-HHMMSS).wav », avec le moteur entendu, ses réglages, la salle et le limiteur, à la fréquence de la carte, en tâche de fond. C'est le même rendu que celui de Ctrl+S, mis en commun (`renderInBackground`).
  - *Rangement.* Rename… renomme tous les fichiers de la prise et refuse d'écraser une prise existante. Delete envoie à la corbeille, après confirmation, et jamais d'effacement définitif. « Show in folder » ouvre l'explorateur sur le fichier.
  - *Test* (autotest 8i) : un WAV de test lu par l'hôte audio ressort au bit près (moteur silencieux). En A/B, A silencieux et B une sinusoïde, le passage à B en cours de lecture donne 0 avant et 0,177 RMS après, comme attendu. La lecture s'arrête d'elle-même à la fin du fichier.

**L'écoute au casque, binaurale (7 novembre 2026).**
  - *Le problème.* Le son du moteur est fait pour deux haut-parleurs : au casque, chaque oreille n'entend que son canal. Le piano est alors dans la tête, le grave collé à gauche et l'aigu à droite. Devant des haut-parleurs, chaque oreille entend les deux, l'une plus tard et plus sourde que l'autre.
  - *Le modèle* (`pf::Room`, réglage `headphones` : fenêtre Room, « Heard on », ou Ctrl+H ; dans la page web, onglet Room). La tête est une sphère de 8,75 cm de rayon. Les deux côtés du son du moteur deviennent deux sources placées à l'angle sous lequel on voit le piano : 37° de part et d'autre de l'axe au clavier, 14° à 3 m, 5° à 8 m.
    - *L'oreille la plus éloignée* entend chaque source plus tard, du retard de Woodworth (a/c)(θ + sin θ) : 0,32 ms au clavier, 0,66 ms pour une source sur le côté.
    - *L'ombre de la tête* suit le modèle du premier ordre de Brown et Duda (un pôle à 2c/a, un zéro qui dépend de l'angle entre la source et l'axe de l'oreille). Ses gains sont pris par rapport à une source de face, parce que les casques sont égalisés pour elle : un son mono garde son niveau (−0,3 dB sous 300 Hz, −0,7 dB de 2 à 8 kHz, mesurés).
    - *Un seul filtre de séparation* (1,65 kHz) découpe graves et aigus. Chaque chemin applique ensuite son gain aux aigus, ce qui reproduit exactement l'étagère de Brown et Duda.
    - *Dans les graves*, les deux côtés arrivent à chaque oreille à peu près pareils. Un gain qui suit la corrélation du son (sur 200 ms) garde l'énergie de chaque oreille : un son mono passe à son niveau ; deux côtés sans rapport donnent chacun la moitié de leur énergie à chaque oreille.
  - *Les réflexions précoces* arrivent aux oreilles, et non plus aux deux micros espacés de 0,6 m. Chaque source image a sa direction depuis la tête, donc son retard et son ombre pour chaque oreille. L'énergie des prises est normalisée pour les deux oreilles ensemble, ce qui garde leurs différences.
  - *La queue de réverbération* reçoit la cohérence entre les oreilles d'un champ diffus pour deux points distants de 2a. La valeur exacte, sin(kd)/kd, est approchée par 1/(1 + (f/590 Hz)²), qui s'en écarte au-delà du premier zéro (980 Hz) en restant positive. Le milieu prend, dans les graves, l'énergie que le côté cède : le milieu passe dans (1 + (√2 − 1)·passe-bas), le côté dans un passe-haut. L'énergie de chaque oreille est conservée exactement à toute fréquence, pour deux sorties non corrélées.
  - *Ce qu'on garde.* Rien n'est retardé sur le chemin de l'oreille la plus proche : aucune latence. Le passage entre haut-parleurs et casque se fait sans clic, par un fondu de 20 ms sur le son direct et de 10 ms sur les réflexions. Sur haut-parleurs, la salle reste identique au bit près. Salle éteinte, le son du moteur est rendu au casque depuis le clavier. Le réglage s'applique à ce qu'on entend, enregistre et diffuse, et au rendu des prises. Il relève de l'ordinateur et non de l'instrument : il n'entre ni dans les préréglages ni dans l'export des réglages.
  - *Mesuré* (test 13 de `room_test`, natif et sous Node) :
    - *retard entre les oreilles*, d'un seul côté : 0,328 ms au-dessus de 3 kHz (Woodworth : 0,317), 0,451 ms sous 500 Hz. Dans les graves, la diffraction de la tête allonge le retard vers la valeur de Kuhn, 3(a/c) sin θ = 0,459 ms ; la phase minimale de l'ombre le donne d'elle-même, comme le notent Brown et Duda ;
    - *différence de niveau* : 0,2 dB sous 300 Hz, 12,8 dB de 4 à 8 kHz ;
    - *mono* : les deux oreilles identiques au bit près ;
    - *queue* : cohérence 0,95 entre 50 et 200 Hz (0,11 sur haut-parleurs), 0,11 de 2 à 6 kHz ; même énergie que sur haut-parleurs (+0,04 dB pour la queue, −0,5 dB pour les 80 premières ms) ;
    - *fonctionnement* : indépendant de la taille des blocs ; pas de clic au changement ; contournement au bit près 276 ms après le retour aux haut-parleurs ; coût multiplié par 1,2.
  - *Limites.* Pas d'élévation, ni de pavillon de l'oreille, ni d'HRTF mesurée : la tête est sphérique, sans les indices spectraux du pavillon. Le gain du direct fluctue avec la corrélation du son sur 200 ms, comme le rétrécissement le faisait déjà sur haut-parleurs. Un enregistrement fait au casque est fait pour le casque.

**Version installable et mises à jour (7 novembre 2026).**
  - *Installateur* (Inno Setup 6, `installer/`, cible `installer`) : `PianoForge-0.1.0-setup.exe`, 5 Mo. La version vient d'un seul endroit, le `project()` du `CMakeLists.txt` racine ; on la retrouve dans l'application (About), dans l'installateur et dans le manifeste.
    - *Contenu* : l'application, le runtime Visual C++ posé à côté d'elle, et en option le plugin OBS (proposé si OBS est installé ; s'il est chargé, il est déplacé de côté comme le fait `install-obs-plugin`). Le runtime à côté de l'exe évite un redistribuable à installer avec des droits d'administrateur : Microsoft autorise ce déploiement local, et l'UCRT fait partie de Windows 10.
    - *Installation* pour l'utilisateur courant, sans droits d'administrateur (`%LOCALAPPDATA%\Programs\PianoForge`), ou pour tous si on le choisit. Une mise à jour reprend le choix fait la première fois, ainsi que le dossier.
    - *Garde-fou du corpus.* La préparation n'accepte que six fichiers nommés, refuse tout nom qui évoque le corpus, un profil, du FLAC ou du WAV, et refuse un exe de plus de 40 Mo, qui embarquerait des données. Le profil calé sur Noire ne peut donc pas partir dans un installateur. L'application installée demande son corpus au premier lancement.
    - *Préservé* : les réglages (`%APPDATA%\PianoForge`), les prises et les enregistrements ne sont touchés ni par une mise à jour ni par une désinstallation.
    - *Essai* (dans un dossier temporaire, sans raccourci ni plugin OBS) : installation silencieuse ; l'exe installé démarre avec le runtime à côté de lui ; mise à jour par-dessus avec `/relaunch=1`, qui garde le dossier, remplace et rouvre l'application ; désinstallation qui ne laisse ni fichier ni entrée de registre, et garde les réglages.
    - *Pas de signature de code* : SmartScreen avertira au premier lancement de l'installateur. Signer demande un certificat, à prévoir avant toute diffusion.
  - *Mises à jour, anticipées* (`UpdateChecker`, menu Help). Il n'y a pas encore de dépôt distant : l'adresse est un réglage, vide par défaut. Sans elle, rien n'est cherché et rien ne quitte l'ordinateur.
    - *Deux formats.* Le manifeste écrit par la cible `installer` (`latest.json` : version, url relative ou absolue, SHA-256, taille, notes) ; ou la dernière release GitHub (l'API `releases/latest` ; l'asset `-setup.exe`, son `digest` ou un asset `.sha256`). Le dossier de sortie peut donc être servi ou partagé tel quel, ou les fichiers déposés dans une release GitHub.
    - *Sécurité.* https ou fichier local seulement (http vers cet ordinateur pour les essais). Un installateur sans SHA-256 est refusé. Taille et SHA-256 sont vérifiés après le téléchargement, et un téléchargement qui ne correspond pas est supprimé. Seul le nom et la version de l'application sont envoyés (User-Agent). Sans signature, l'empreinte protège contre un téléchargement abîmé, pas contre un serveur compromis : la confiance repose sur l'adresse.
    - *Déroulé.* Help > Check for updates, ou au démarrage, au plus une fois par jour, si on l'a coché. Une version plus récente est proposée : installer, ignorer cette version (elle ne sera plus proposée au démarrage, seulement sur demande) ou plus tard. Le téléchargement se fait en tâche de fond, avec sa progression dans la barre d'état. Ensuite l'installateur est lancé (silencieux, sans boîte de dialogue, sans redémarrage, en fermant ce qui utilise ses fichiers, puis il rouvre PianoForge), et l'application se ferme proprement, ce qui libère le pilote ASIO. Pendant un enregistrement, rien n'est installé : il faut d'abord l'arrêter.
    - *Tests* : autotest 8j (ordre des versions, préversions comprises ; manifeste lu depuis une adresse de fichier, installateur relatif téléchargé et vérifié ; mauvais SHA-256 et mauvaise taille refusés sans rien laisser ; format GitHub ; http refusé). `PianoForge.exe --check-update <adresse> rapport.txt` lit le manifeste réellement produit et vérifie l'installateur sans le lancer.

**Résonance sympathique par note (7 novembre 2026).** Une forme d'excitation par registre laissait des écarts de 10 dB. Un essai par note sur une partie des notes généralisait mal aux autres (validation en laissant une note de côté : 90e centile 10,3 dB, contre 8,45 pour les registres). On a donc mesuré toutes les notes.
  - *Mesure.* Le banc a rendu les paires pédale enfoncée / pédale relevée des 88 notes à v40 et v100 (176 paires, environ 75 s pour 300 prises). Pour chaque paire, la réponse du modèle a été calculée pour 25 formes d'excitation (montée de 0,05 à 0,5 s, décroissance nulle ou de 0,5 à 4 s) : 5 h 40 sur 11 cœurs.
  - *Ajustement* (`forge sympathetic`, `fit_notes`). Pour chaque note jouée, on cherche la forme, le gain et l'exposant de vélocité qui s'accordent le mieux à ses deux prises. Ils s'ajoutent aux gains par bande et à l'exposant commun ; une régularisation retient l'exposant, qui ne repose que sur deux vélocités.
    - Les gains vont de −5,2 à +6,5 dB (10e et 90e centiles ; extrêmes −13,4 et +16,5). Les exposants vont de −0,17 à 0,67, médiane 0,26.
    - Les formes sont variées : 39 notes montent en 0,5 s, 18 en 0,05 s ; 41 n'ont pas de décroissance propre, 17 décroissent en 4 s.
    - Le profil les porte dans la section SYMP : un registre par note, avec son gain et son exposant. L'export complet, cordes couplées comprises, prend 10 min 25 s.
  - *Validation du moteur* (`symp_validate` : différence pédale enfoncée moins pédale relevée, rendue par le moteur, contre celle de Noire ; 176 prises, 5 fenêtres de 0 à 9 s) :
    - toutes les cases : médiane −0,9 dB, la moitié à moins de 2,5 dB, 90 % à moins de 7,4 dB. Avec les registres, sur les mêmes prises : +0,4 dB, 4,3 et 10,7 ;
    - cases audibles (Noire au-dessus de −90 dBFS, 707 cases) : moitié 2,3 dB, 90 % 5,5 dB (avec les registres : 3,9 et 9,0) ;
    - par registre, moitié et 90 % : grave 2,3 et 5,5 dB (3,7 et 8,6 avant), médium 1,8 et 4,4 dB (4,0 et 9,7 avant), aigu 3,3 dB (5,3 avant). Le 90e centile de l'aigu (22 dB) vient de cases où notre résonance est déjà retombée au silence numérique après 4 s, quand celle de Noire est vers −115 dBFS : c'est inaudible, et c'était déjà le cas.
  - *Limites.* Chaque note est ajustée sur ses propres prises : il n'y a plus d'interpolation entre notes, mais pas non plus de validation hors échantillon. Seules v40 et v100 sont mesurées : entre et au-delà, la vélocité suit l'exposant de la note. Les tests du moteur, l'autotest et la page web passent avec ce profil.

**Le plugin VST3 (7 novembre 2026).** PianoForge dans un DAW (`plugin/`, JUCE, format VST3).
  - *Le même son que l'application.* Le plugin joue la même chaîne : le moteur modal, la salle et la position d'écoute (casque compris), le limiteur.
    - Il suit la même règle de fréquence : au-delà de 50 kHz, le moteur tourne à 44,1 ou 48 kHz et `pf::Upsampler` remonte le son. La latence, 127 échantillons à 192 kHz, est annoncée à l'hôte ; elle est nulle sinon.
    - Les tables (préréglages de salle et positions, tempéraments, format texte des courbes) sont partagées avec l'application (`RoomPresets.h`, `Tuning.h`, `CurveText.h`), pas recopiées.
  - *Paramètres automatisables*, avec les noms et les valeurs par défaut de l'application : volume (+6 dB), les six options du moteur, niveau de la sympathie, unisson, refrappe, salle (volume, décroissance, brillance, largeur, niveau), distance, keep loudness, haut-parleurs ou casque, La, tempérament et tonique, limiteur.
    - Les préréglages de salle et les positions sont des raccourcis de l'éditeur, qui règlent ces paramètres en un geste. « Custom » s'affiche dès qu'on s'en écarte.
    - Le MIDI : notes, CC64 continu, CC66, CC67. En VST3, les contrôleurs n'existent pas comme événements : l'hôte les envoie sous forme de paramètres cachés (2 048, ajoutés par JUCE), que le plugin retransforme en MIDI.
  - *Le profil n'est jamais dans le plugin.* Par défaut, c'est celui du corpus de l'application : ses réglages, lus sans être modifiés, disent où il est. Sinon, c'est un `modal-profile.bin` choisi dans le plugin ; le projet garde son chemin, pas le fichier.
    - Le chargement se fait en tâche de fond : 0,1 s, puis 0,02 s pour une seconde instance, qui partage le même profil en mémoire (un cache par fichier et par version).
    - Pendant l'installation d'un profil, le fil audio est tenu à l'écart par un protocole de Dekker (suspendu / dans le bloc) et rend du silence ; rien ne se verrouille côté audio.
  - *Blocs de toute taille*, jusqu'à un échantillon (FL Studio coupe aux points d'automation). Le MIDI est placé à l'échantillon. Au-delà de 50 kHz, les échantillons rendus mais pas encore donnés attendent le bloc suivant, et un événement qui tombe dessus attend la trame suivante. Un événement placé sur la grille du moteur tombe ainsi sur la même trame, quel que soit le découpage.
  - *`reset()` de l'hôte* (transport relancé, rendu hors ligne) : le moteur a maintenant un `reset()` sans verrou. Au bloc suivant, tout est silencieux (voix, table, cordes libres), les pédales sont relevées et l'ordre d'allocation des voix repart de zéro. La salle, le limiteur et le suréchantillonnage repartent aussi de zéro.
  - *L'éditeur*, au thème de l'application :
    - en-tête : le profil et d'où il vient, avec « Choose profile… » et « Application's profile » ;
    - cartes Sound, Output (limiteur et crête), Room around the piano, Listening position (avec le résumé physique et, au casque, l'angle et le retard interaural), Tuning, Velocity curve ;
    - la courbe de vélocité est propre au plugin et gardée dans le projet : cinq points qu'on tire, la plage de la touche la plus douce à la plus forte, « Linear », et « Application's curve » pour reprendre celle de l'atelier calée sur le B2 ;
    - en bas : la fréquence du moteur, la latence et les voix.
  - *Installation* : la cible `install-vst3-plugin` copie le bundle dans le dossier VST3 par utilisateur (`%LOCALAPPDATA%\Programs\Common\VST3`, que REAPER parcourt chez l'utilisateur), sans droits d'administrateur. Une copie chargée par un hôte est déplacée de côté. L'installateur le propose (case cochée) dans le dossier VST3 commun ou par utilisateur, selon le mode d'installation ; la liste blanche passe à dix fichiers.
  - *Tests* (`plugin_host`, un hôte VST3 JUCE qui charge le vrai `PianoForge.vst3`) :
    - déclaration : un instrument, sans entrée, sortie stéréo, MIDI en entrée, 23 paramètres ;
    - son : identique à celui du moteur appelé directement, à −137 dB sous la crête, pédale comprise. Le CC64 traverse donc bien le passage par les paramètres VST3 ;
    - blocs de 1 à 512 échantillons : même son à −52 dB sous la crête. Le moteur prend quelques décisions aux frontières de ses blocs (début d'un fondu de partiel, mise en veille de la table) ; un autre découpage les déplace de quelques échantillons, ce qui reste inaudible ;
    - sous le plafond du limiteur ; volume automatisé de +6 à 0 dB : −6,00 dB ; casque : −0,74 dB par rapport aux haut-parleurs ;
    - projet rouvert : paramètres et son identiques au bit près, profil partagé avec la seconde instance ;
    - hôte à 192 kHz : latence de 127 échantillons, même niveau qu'à 48 kHz (−0,00 dB), blocs de 1 à 1 024 à −54 dB ;
    - coût : 64 notes pédale enfoncée, salle allumée, 22 % du temps réel.

    Le même test passe sur la copie installée par l'installateur, que la désinstallation retire.
  - *Ce que le test a appris.* Un hôte ne doit pas envoyer plus d'échantillons qu'il n'en a annoncé (VST3 l'interdit) : un premier test qui le faisait corrompait le tas dans la couche VST3 elle-même. Le plugin découpe tout de même les blocs trop longs. Les paramètres d'un plugin hébergé ne connaissent que leur valeur normalisée et leur texte.
  - *Limites.* VST3 seulement (CLAP plus tard). Windows seulement à l'essai (le code est portable). Processeur AVX2 requis par le moteur. Le plugin n'est pas signé.

**Une corde par note : refrappe et polyphonie (7 novembre 2026).** Le principe 2 du §2.3 (« pas de voix, mais 88 notes toujours présentes ; une note rejouée frappe une corde qui vibre encore ; jamais de vol de voix ») n'était pas tenu. Le moteur créait une voix par frappe, effaçait la précédente en 0,8 s, gardait 3 voix par note au plus et 64 en tout.
  - *Mesuré d'abord.* Une séance du banc (`tools/bench/sessions/restrike.toml`, 11 prises rendues par Noire en 1 min 45) :
    - notes répétées pédale enfoncée, à 50, 100 et 200 ms, en crescendo, en decrescendo, dans le grave, le médium et l'aigu ;
    - refrappe touche tenue, et staccato ;
    - glissandos pédale enfoncée, chromatique (88 notes) et sur les touches blanches (52 notes).

    Chez Noire, huit frappes de A3 toutes les 100 ms montent à +8,7 dB, à peu près la superposition (+9 dB pour huit frappes égales). Le moteur plafonnait à +4 dB et restait 5 à 11 dB sous Noire. Le glissando chromatique volait 24 voix : une fois les touches relâchées, pédale tenue, il manquait 5,5 dB sous 250 Hz et 4,1 dB entre 250 et 1 000 Hz.
  - *Le modèle.*
    - *Une voix par note* : `voices[note]`, 128 places, posées sur le tas (128 voix ne tiennent plus sur une pile). Plus aucun vol de voix.
    - *Le marteau frappe une corde en mouvement.* Ce que la corde joue à cet instant (chaque mode, à son niveau, enveloppe d'étouffoir comprise) continue de sonner, placé après les modes de la nouvelle frappe. Pendant le contact du marteau (3 ms), ces modes perdent la part absorbée par le feutre (`restrikeAbsorption`), par un amortissement de leurs pôles : rien ne saute dans la forme d'onde.
    - *À la fin du contact*, chaque ancien mode rejoint le mode nouveau du même partiel dont le pôle est le plus proche (fréquence et amortissement). Avec la même couche, c'est le même mode : sans absorption, les frappes s'additionnent exactement. Les partiels absents de la nouvelle couche gardent leurs propres modes, et le résidu d'attaque de la frappe précédente va jusqu'à son terme (4 résidus par note).
  - *Deux essais écartés par la mesure.*
    - Répartir la somme d'un partiel sur les nouveaux modes selon leurs amplitudes initiales versait le son rémanent (les modes lents, l'essentiel de ce qui reste) dans les modes rapides de la nouvelle frappe, qui l'éteignaient. La montée restait trop faible pour A3 (+6 dB contre +8,7) et devenait trop forte pour C2 (+11,7 contre +7,1).
    - La superposition exacte (pôle le plus proche, sans absorption) suit Noire pendant les quatre premières frappes : +4,0, +6,4, +7,8 et +8,8 dB, contre +4,2, +6,5, +8,1 et +8,7. Mais elle continue de monter, et la tenue de pédale qui suit est 5 à 7 dB trop forte : Noire ne superpose pas entièrement, elle éteint ses anciennes voix.
  - *L'absorption, calée sur Noire.* Le balayage de 0 à 92 % sur les neuf prises répétées donne 50 %, où l'écart moyen par prise est le plus faible. Toutes prises confondues, l'enveloppe (fenêtres de 50 ms où Noire dépasse −80 dBFS) a un écart médian de −0,2 dB, la moitié dans 1,9 dB et 90 % dans 7,7 dB, contre −4,6, 4,7 et 11,6 dB avant. Par prise, l'écart médian va de −1,6 à +1,7 dB.
    - Ce qui reste : pendant des répétitions rapides, notre montée est de 2 à 3,5 dB sous celle de Noire (A3 toutes les 100 ms : +5,2 contre +8,7 dB). Noire garde ses voix à plein niveau puis les éteint ; une seule absorption physique ne peut pas reproduire à la fois la montée et la tenue qui suit.
    - Le réglage est dans Engine settings (« Re-strike absorption »), dans le plugin et dans la page web. Le lecteur de référence garde son fondu de refrappe, renommé en conséquence.
  - *Polyphonie.* Glissando chromatique pédale enfoncée : 88 voix, aucune volée. Une fois les touches relâchées, pédale tenue, on est à −0,7 dB de Noire sous 250 Hz (au lieu de −5,5), −0,5 dB de 250 à 1 000 Hz et +0,6 dB de 1 à 4 kHz. Au-dessus de 4 kHz, −2,7 dB, comme pour le glissando sur les touches blanches qui ne volait rien : c'est un écart général des aigus, pas la polyphonie. Coût : 88 notes pédale enfoncée, 20,1 % du temps réel (64 : 17,5 %).
  - *Tests* (`modal_engine_test`, 44 vérifications) :
    - à l'échantillon de la refrappe, la sortie est exactement ce que la corde allait jouer plus le premier échantillon de la nouvelle frappe, avec une seule voix pour la note ;
    - sans absorption, huit frappes donnent la somme des frappes isolées (+0,00 dB) ;
    - avec 50 %, ce qui reste de la première frappe après le contact vaut 0,7071 d'elle-même (√0,5), sans marche ;
    - un glissando de 88 notes pédale enfoncée donne la somme des notes jouées seules (−131 dB), sans voix volée.

    Les tests du plugin, de l'application et de la page web passent aussi.
  - *`modal_render`* affiche le maximum de voix actives et les voix volées, et accepte `--absorb`. L'option `--restrike` a disparu avec le fondu.

**La vélocité entre deux couches (7 novembre 2026).** Le moteur jouait la couche mesurée la plus proche, au gain global interpolé entre les deux couches voisines ; après le résidu, chaque partiel rejoignait son niveau interpolé. Le niveau et le timbre sautaient donc aux frontières entre nos 16 couches.
  - *Mesuré d'abord*, sur la passe de découverte des couches (A0, C2, C3, C4, C6 et C8 aux 127 vélocités, contre Noire) :
    - *l'attaque* (60 premières ms) : jusqu'à +10,5 dB au-dessus de Noire (C8 v20), et des sauts de niveau aux frontières ;
    - *l'entretien* (0,5 à 1,5 s) : un saut de −4,7 dB à C2 v100→101, +3,3 dB à A0 v36→37, −2,2 dB à C6 v108→109, quand Noire ne dépasse pas 2,5 dB ;
    - *Noire elle-même procède par couches*, une vingtaine, au timbre identique au sein d'une couche : ses propres marches de timbre d'attaque ont une médiane de 1,9 dB, comme les nôtres.
  - *Trois essais écartés.*
    - *Mélanger les deux frappes* (à puissance constante) : d'une couche à l'autre, les phases des partiels n'ont aucun rapport (écart médian de 80 à 165°). La somme annule certains partiels et en renforce d'autres, selon la vélocité.
    - *Égaliser le seul résidu* vers l'autre couche : le résidu annule les modes extrapolés jusqu'à la frappe, 16 à 32 dB au-dessus de l'enregistrement dans les 5 premières ms. Il ne les annule qu'au même gain et à la même phase ; filtré seul, il laisse un éclat.
    - *Filtrer toute la frappe* (dix bandes d'octave), en appliquant aux modes la réponse du filtre à leurs pôles : c'est la réponse établie, sans le transitoire propre du filtre, aussi grand que les modes extrapolés. L'annulation tombe de nouveau.
  - *Ce qui est retenu.*
    - *Pendant le résidu, un seul gain sur toute la frappe* (modes, table, résidu), le seul changement qui garde l'annulation. Il amène l'énergie de l'attaque telle qu'enregistrée (60 ms) à celle interpolée en dB entre les deux couches. Le niveau de l'attaque suit la vélocité ; son timbre reste celui de la couche la plus proche et change là où elle change, comme chez Noire à ses propres couches.
    - *L'énergie de l'attaque compte la table.* Le résidu a été mesuré contre les cordes et la table : à A0 v8, la table excitée en annule 10 dB. Sans elle, le saut à A0 v12→13 restait de +8,3 dB.
    - *Après le résidu*, chaque partiel rejoint en 50 ms son niveau interpolé. Puis chaque mode prend la fréquence et l'amortissement déplacés vers le mode le plus proche du même partiel dans l'autre couche (fréquence linéaire, amortissement en log). Les partiels de l'autre couche seule entrent pendant la rampe depuis −60 dB, ceux de la couche proche seule s'effacent vers l'autre.
    - *Le niveau interpolé est celui que le partiel fait entendre* sur l'entretien (0,3 à 2 s) : la valeur efficace de la somme de ses modes, termes croisés compris. Le niveau du profil (la somme des énergies des modes) s'en écarte quand deux modes forts d'un partiel s'annulent, comme la forge en ajuste parfois sur un unisson. Partiel 2 de C2, couche 104 : deux modes à −30,5 et −30,6 dB, 12,5 dB au-dessus de la couche 96 selon le profil, 3 dB selon le son. C'était le saut de −4,7 dB.
    - *Un partiel dont les modes interfèrent garde ses pôles.* Déplacer les fréquences change le battement des modes d'un partiel. À C4 v116, les cinq modes du partiel 6 tiennent dans 1,6 Hz ; déplacés de 0,27 Hz au plus, ils ne s'annulaient plus comme mesuré, et le partiel sortait 10 dB trop fort. Le chargeur compare donc, pour chaque partiel, son niveau tel qu'il sonne à celui de la somme des énergies de ses modes sur la même fenêtre. S'ils diffèrent de plus de 1 dB, les modes interfèrent : le partiel garde les pôles de la couche proche, et son niveau est exactement celui qui est mesuré. Pour les autres partiels, la correction des pôles déplacés part des énergies de leurs modes, avec le moment où ils bougent (la fin de la rampe) : un mode rapide (21,6 /s) ralenti vers 5 /s n'a alors presque plus d'énergie.
    - *Un essai écarté pour son coût :* recalculer à chaque frappe, en forme close, le niveau exact de chaque partiel aux pôles déplacés. C'était juste, mais un accord de 10 notes entre deux couches coûtait 0,8 ms dans un bloc de 64 échantillons (1,33 ms), contre 0,19 ms avant. Avec la règle retenue, c'est 0,24 à 0,30 ms. Le pôle déplacé se déduit aussi du pôle mesuré par une série (le décalage est minuscule), sans nouvelle exponentielle.
    - *Les mesures des couches viennent de la forge* (`forge export`, section `LEVL` du profil) : l'énergie de l'attaque et le niveau de chaque partiel sur l'entretien. Calculées au chargement, elles prenaient 3,2 s en natif et 5,6 s dans la page web, où le profil se charge sur le fil audio du worklet ; le moteur s'installe maintenant en 10 ms. Un profil plus ancien joue comme avant entre deux couches (gain global sur l'attaque, niveaux du profil ensuite). `pf::measureLayers` refait le même calcul en C++ : pour les profils construits en code des tests, et pour contrôler la forge. Sur les 1 406 couches et les 33 026 partiels du profil de référence, les deux concordent à 0,0001 dB près.
  - *Résultats*, sur les six notes aux 127 vélocités, contre Noire :
    - *attaque* : le plus grand saut de niveau entre deux vélocités voisines est de 0,4 à 0,5 dB sur chaque note, contre 1,0 à 4,0 dB avant (17 dB à C8 v20→21) ; l'écart à Noire reste dans −1,5 à +1,5 dB au-dessus de la couche la plus douce. En dessous (v1 à v7), la pente extrapolée laisse C8 jusqu'à 5,7 dB au-dessus de Noire, comme avant ;
    - *entretien* : le plus grand saut est de 0,9 dB à A0 (3,3 avant), 0,6 dB à C2 (4,7), 0,4 dB à C3 et C4 (0,9 et 0,8), 0,9 dB à C6 (2,2). C'est sous les marches de Noire elle-même (jusqu'à 2,5 dB à C2, 2,6 dB à C6). L'écart de timbre à Noire entre les couches a une médiane de 1,06 dB (1,13 avant). Au-delà de 0,5 s, C8 est sous −100 dBFS, au plancher de bruit de Noire : ses chiffres n'y signifient rien ;
    - *le timbre de l'attaque* change toujours aux frontières de nos couches (médiane 1,9 dB, 90 % sous 3,8 dB), comme celui de Noire aux siennes : c'est le prix de l'annulation du résidu.
  - *Tests.*
    - `modal_engine_test`, 52 vérifications. Entre deux couches : l'attaque est la frappe de la couche proche au gain d'attaque, à −90 dB près ; son niveau suit la vélocité sans marche (0,14 dB par pas, comme l'interpolation, contre 3,6 dB au gain global seul) ; un mode prend la fréquence et l'amortissement déplacés (1 502,500 Hz, 6,639 /s pour 6,640) ; un partiel de l'autre couche seule entre depuis −60 dB, à son niveau ; un partiel aux modes qui s'annulent (profil : 14,8 dB d'écart entre les couches, son : 0,3 dB) a le même entretien des deux côtés de la frontière.
    - Côté forge (`tests/test_export.py`), les mesures sont vérifiées contre une synthèse indépendante et une intégration numérique.

    Les tests du plugin, de l'application et de la page web passent aussi.
  - *`modal_render --nearest`* joue sans la fusion des couches (la couche la plus proche au gain global), pour comparer.

**Le plugin OBS, pour le stream (7 novembre 2026).** Dix points, demandés ensemble. Le plugin ne donnait que le son, tel qu'entendu.
  - *La mémoire partagée, version 2* (`PianoForgeFeed`, `pf::AudioFeedWriter`). Elle porte :
    - trois mix des mêmes trames, avant le volume et le limiteur de l'application : tel qu'entendu, sur enceintes, sec ;
    - le volume de l'application ;
    - l'instant de chaque bloc, sur l'horloge stable de la machine (la même dans tous les processus) ;
    - les évènements MIDI à leur trame ;
    - l'état pour les incrustations, et des marques ;
    - huit emplacements de lecteurs, où chaque source dit ce qu'elle lit et avec quel retard.

    Le nom change : un ancien plugin qui garderait l'ancienne mémoire ouverte ne bloque pas la nouvelle. L'application et le plugin vont donc ensemble.
  - *1. Le mix et le niveau du stream.* Au casque, OBS recevait le rendu binaural, fait pour des écouteurs et coloré sur des enceintes, et le volume d'écoute changeait le niveau du stream.
    - Le volume passe après la salle, dans l'hôte audio ; le flux prend le son avant lui.
    - Le mix « sur enceintes » (par défaut) vient d'une seconde salle, aux réglages de la première sans la tête. Elle tourne seulement quand une source OBS prend ce mix, et part des réglages courants, sans glisser vers eux. Sur enceintes, elle redonne la salle entendue à l'échantillon près.
    - Le plugin a son gain, suit ou non le volume de l'application (non par défaut), et a son propre limiteur sous −1 dBFS.
  - *2. Le test instable.* La fréquence s'écartait de 50 à 350 ppm sur des fenêtres d'une seconde. La boucle PI corrigeait le rapport d'après le remplissage du tampon, une dent de scie déchiquetée quand les blocs arrivent en rafales (une minuterie de 15,6 ms) ; une seconde de moyenne ne la lissait pas, et la boucle errait.
    - Le débit de l'application est désormais mesuré sur les instants de ses blocs : une droite des moindres carrés, uniforme au démarrage puis pondérée sur une minute.
    - Le rapport nominal suit ce débit, et la boucle, purement proportionnelle et calme, ne tient plus que le remplissage.
    - Le remplissage est compté depuis les instants des trames : la dent de scie est retirée, sa moyenne gardée.
    - Résultat : quatre passages sur quatre, la fréquence à ±8 ppm (tolérance ±50), le résidu après ajustement d'une sinusoïde à −62 dB.
    - Trois essais écartés en route. Ajouter la dent de scie sans retirer sa moyenne déplaçait la consigne d'une demi-rafale : la boucle partait en butée, puis en sous-alimentation, en boucle. Garder l'intégrale avec le débit mesuré, ou la compenser à chaque mise à jour, annulait l'apport de la mesure ou figeait un écart du démarrage. La moyenne exponentielle seule surpondérait le premier point (−300 ppm à 5 s).
  - *3. Des fondus.* Tout démarrage monte en 10 ms. Tout arrêt prévu descend en 10 ms : nouveau mix, nouvelle latence ou nouveau format, bouton de resynchronisation, audio de l'application arrêté. Pour ce dernier cas, un bloc de l'application est en retard quand il dépasse une fois et demie le plus long intervalle récent entre blocs : avec un seuil fixe, les rafales passaient pour des arrêts et coupaient le son.
  - *4. La source « clavier ».* Les 88 touches éclairées selon la vélocité, les trois pédales, et des traînées qui montent des touches tant qu'elles sont tenues. Elle montre la trame que la source audio horodate à cet instant : l'image rejoint le son. Sans source audio, elle suit l'application, 20 ms derrière.
  - *5. La latence de la touche à OBS*, dans l'état de la source : l'âge des trames horodatées, d'après les instants de l'application. Dans le test, 18 à 26 ms pour une latence de 20 ms. Elle sert à régler le décalage de synchronisation face à une caméra.
  - *6. Des marqueurs de chapitre* dans l'enregistrement d'OBS, à chaque prise sauvegardée, enregistrement lancé ou arrêté et fichier MIDI joué (API du frontend d'OBS, sur le fil de l'interface ; OBS 30.2 et plus récent, MP4 hybride). Quand OBS n'enregistre pas, le journal le dit.
  - *7. La source « texte »* : un modèle dont les champs ({file}, {state}, {preset}, {room}, {engine}, {recording}) prennent l'état de l'application. Elle est dessinée par la source texte d'OBS elle-même, dont les propriétés (police, couleurs, contour) restent disponibles. Une ligne dont tous les champs sont vides disparaît.
  - *8. Une seule conversion de fréquence* : le plugin sort directement à la fréquence d'OBS, dans le même rééchantillonneur que la correction de dérive.
  - *9. Le signal de vie* : la carte Session de l'application dit ce qu'OBS lit (son, clavier, texte) et la latence de la touche à OBS, ou qu'aucune source ne lit le flux.
  - *10. Un état qui se rafraîchit seul* (à chaque changement de ce qu'il dit, et toutes les cinq secondes en marche, pour les mesures) et des textes traduisibles, en anglais et en français.
  - *Tests.*
    - Le test du plugin dans libobs démarre désormais aussi la vidéo (module Direct3D 11 d'OBS, hors écran) et charge la source texte d'OBS. Il vérifie le son (mix, niveaux, fondus, fréquence, propreté), l'état, les lecteurs, le rendu du clavier pixel par pixel, le texte et les chapitres.
    - Un test sans OBS couvre la logique des incrustations, et le test de la mémoire partagée ses nouveautés.
    - L'autotest de l'application vérifie les trois mix à l'échantillon près.
  - *Compléments après l'essai de l'utilisateur.*
    - *Couleurs du clavier* : par main (bleu et bleu foncé, orange et orange foncé, le foncé pour les touches noires), par type de touche, ou une seule. L'estimation des mains part des notes seules. Chaque main a une place, et un groupe de notes frappées ensemble (35 ms, déjà en file, le son étant en retard) va à la main qui lui coûte le moins : distance à sa place, écart qu'il lui demanderait, croisement des touches de l'autre main. Un accord trop large pour une main (plus de 14 demi-tons) est partagé à son plus grand intervalle. Un partage fixe reste possible.
    - *Effets lumineux* : un halo qui monte des touches enfoncées, un éclair vers le blanc à la frappe (selon la vélocité, un dixième de seconde) et une rémanence quand la touche remonte.
    - *L'état de la source audio* faisait un pavé, reconstruit toutes les cinq secondes : sa hauteur changeait et déplaçait les boutons sous la souris. Il tient en deux lignes, en bas des propriétés, et ne se reconstruit que quand l'état change. L'explication sur la caméra est passée dans l'aide.
    - *101 resynchronisations en 2 min 30* : ce n'était pas le plugin. Le FiiO absent, l'application retentait son pilote ASIO toutes les deux secondes, sans fin : il reste listé sans son appareil, ce qui remettait le compteur de tentatives à zéro. Chaque tentative coupait ASIO4ALL, et le son de l'application sautait. Désormais : huit tentatives, puis une toutes les trente secondes et seulement dans le silence ; le compteur repart quand l'appareil revient vraiment dans la liste. Le plugin compte les resynchronisations par session audio de l'application.

**Gestes au piano et enregistrement à la première note (7 novembre 2026).** Pour ne plus aller cliquer sur Record, et ne plus enregistrer de vide.
  - *Les gestes* (`pf::PianoGestures`, Keyboard > Piano gestures). Ce sont des touches frappées ensemble qu'aucune pièce ne demande, ou une touche frappée deux fois vite. Un accord compte si ses touches, et elles seules, arrivent en 0,15 s et sont tenues ensemble ; un double appui, si la touche est frappée, relâchée et refrappée en 0,35 s, sans autre note entre. Une mélodie sur les mêmes touches, un trille ou une note répétée lentement ne déclenchent rien.
    - On apprend un geste en le jouant. Un geste ne fait qu'une chose : donné à une autre action, il quitte la première.
    - Ils ne viennent que du clavier MIDI, pas de la souris ni d'un fichier.
    - Par défaut : la0 + la#0 + si0 arme ou arrête l'enregistrement ; do8 deux fois sauvegarde la prise ; la0 deux fois lance ou met en pause le fichier MIDI ; la#7 + si7 + do8 lance ou arrête l'enregistrement d'OBS. Le stream et les scènes d'OBS restent sans geste, puisque partir en direct par erreur coûte trop cher.
  - *Les notes d'un geste sonnent* : on ne peut le reconnaître qu'une fois joué. Mais un enregistrement arrêté par un geste s'arrête juste avant sa première note, et une prise sauvegardée par un geste s'arrête avant lui, les notes encore tenues relâchées là. Pour cela, l'écriture du WAV garde une demi-seconde de recul.
  - *Vers OBS*, un canal de commandes s'ajoute en fin d'en-tête de la mémoire partagée, ce qui garde le plugin installé compatible. Le plugin les exécute sur le fil de l'interface d'OBS (enregistrement, stream, chapitre, scène suivante ou précédente), si sa source audio le permet.
  - *L'enregistrement armé* (File > Start recordings at the first note, par défaut). Record arme ; la première note qui atteint le moteur le déclenche, et il commence 0,25 s avant elle. L'enregistreur garde ce quart de seconde en mémoire pendant l'attente. Sans note, rien n'est enregistré. Le bouton affiche « Armed: play », la carte Session aussi, et le marqueur de chapitre pour OBS part au vrai début.
  - *Le flux vers OBS sous charge.* Le fil de lecture du plugin passe en priorité haute, comme les fils audio d'OBS. Le débit de l'application se mesure sur l'enveloppe basse des instants de ses blocs : un bloc peut arriver en retard, jamais en avance, et chaque demi-seconde on garde le moins en retard. Avec OBS et PianoForge ouverts à côté, le test garde la fréquence à ±3 ppm.
  - *Après l'essai* :
    - un enregistrement armé par un geste démarrait aussitôt. Le geste est reconnu à sa dernière note, et l'armement arrivait avant que cette note n'atteigne le moteur : elle le déclenchait. L'enregistrement armé ignore désormais les notes des 100 ms qui suivent le geste ;
    - le clavier à l'écran montre les gestes : un point sous chaque touche d'un accord-geste, deux sous une touche à frapper deux fois. Au survol, une bulle dit le geste et l'action, et les autres touches du geste s'éclairent. L'application n'avait pas de fenêtre de bulles d'aide (`TooltipWindow`) : aucune de ses bulles ne s'affichait. Elles s'affichent maintenant partout.
  - *Tests* : `piano_gestures` (accords, doubles appuis, musique qui ne déclenche rien, apprentissage) ; dans l'autotest, l'enregistrement armé (départ 0,250 s avant la note, arrêt coupé à la position demandée, WAV identique à l'échantillon près, rien sans note) ; les commandes dans le test de la mémoire partagée.

**Le clavier OBS redessiné (9 novembre 2026).** Le rendu était fait de rectangles pleins et de fondus : « carré et moche ». L'utilisateur voulait de l'élégance, de l'animation, des paillettes, de vraies lumières, des notes arrondies avec un bord, et des couleurs choisies par teinte.
  - *Un shader du plugin* (`obs/data/pianoforge-keyboard.effect`). L'API de dessin d'OBS n'offre que des rectangles unis. Le shader dessine des boîtes arrondies par distance signée : dégradé, liseré, lumière venue du haut ou du bas, bord lissé (ou flou, pour une ombre). Il y ajoute des halos, des faisceaux et des paillettes. Il travaille en pixels de la source, si bien qu'une forme coupée au bord de la source est rognée sans être écrasée : rien ne déborde sur la scène.
  - *Le clavier.* Les touches blanches ont un avant arrondi et un rebord qui s'enfonce avec la touche ; les noires ont un corps, une face qui s'allonge quand on appuie, un reflet, et une ombre portée sur les blanches. Le tout est posé sur un lit sombre, sous un rail à liseré de feutre. Une touche enfoncée prend sa teinte en dégradé et s'éclaire de l'intérieur à la frappe.
  - *Les notes* sont des barres arrondies, avec un liseré pâle et un halo, plus opaques quand la frappe est forte. Elles s'estompent dans le dernier tiers de leur montée, et celles des touches noires, plus étroites, passent devant.
  - *Les lumières* s'ajoutent à l'image, au lieu de la recouvrir. Un faisceau scintillant monte des touches tenues. À la frappe, un éclat jaillit au bord de la touche et un reflet court le long du rail.
  - *Les paillettes.* À chaque frappe, une gerbe jaillit, d'autant plus fournie, rapide et durable que la vélocité est forte : de 3 à 18 étincelles à 100 %. Quelques-unes montent encore tant que la touche est tenue. Elles ralentissent dans l'air, continuent de s'élever un peu, oscillent, scintillent et s'éteignent ; la moitié a la teinte de la touche, l'autre une nuance plus pâle. Une frappe vue avec plus d'un quart de seconde de retard (un rattrapage) n'en lance pas.
  - *Les couleurs par teinte* : un curseur de 0 à 360° et une saturation. Il y a quatre modes : par main (217° et 25° par défaut, le bleu et l'orange d'avant), touches blanches et noires, une teinte pour toutes, ou un arc-en-ciel le long du clavier. Les touches noires prennent une nuance plus profonde de leur teinte, calculée au lieu d'être choisie. Les huit sélecteurs de couleur disparaissent.
  - *Ce que le test a révélé.* À la fin de chaque technique, OBS efface les valeurs de tous les paramètres de l'effet. Un paramètre réglé une seule fois par image (le temps, la bande d'estompage) n'arrivait donc qu'au premier shader dessiné. Pour les autres, Direct3D refusait le dessin (« Not all shader parameters were set ») : aucune paillette, ni les premières notes de la première image. Chaque forme règle désormais tous les paramètres que son shader lit, et le test vérifie que Direct3D ne dit rien.
  - *Le coût.* Plusieurs centaines de formes par image. Le dessin garde la technique ouverte tant que des formes du même genre se suivent, et n'envoie que ce qui change. Toutes les formes sont tracées sur un même carré unité, au lieu de réécrire le tampon de sommets de `gs_draw_sprite` à chaque fois. Les paillettes partent par 128, leurs sommets portant couleur, éclat et phase. Une image chargée (quinze notes, leurs lumières, des paillettes) passe ainsi de 2,0 à environ 1 ms du fil graphique d'OBS.
  - *Après l'essai* : les paillettes partaient « comme une explosion ». Toutes quittaient la touche au même instant, vite, dans tous les sens, freinées net puis poussées vers le haut. Désormais une frappe les lâche en un court filet, sur un quart de seconde. Chacune part juste assez vite pour atteindre sa hauteur quand elle s'éteint, et ralentit doucement. Trois réglages s'ajoutent :
    - la hauteur, en part de la place au-dessus des touches (70 %, moins pour une frappe douce) ;
    - l'ouverture du cône de départ, de 0° (un fil droit) à 90° (une fontaine) ; 20° par défaut ;
    - la direction, une inclinaison de −45° à 45°, comme une brise ; droites par défaut.
  - *Tests* : couleurs par teinte, frappes, filets de paillettes (montée droite dans la largeur de la touche, jamais plus haut que demandé, inclinaison, départs échelonnés, extinction, plafond) dans le test sans OBS. Dans libobs, en plus, des paillettes penchées à droite se décalent bien vers la droite. Dans libobs : couleur de chaque main, note montante, pédale, phrase jouée (images gardées), paillettes présentes ou absentes selon le réglage, arc-en-ciel, propriétés, temps de rendu et silence de Direct3D. Le shader est installé à côté des langues, par `install-obs-plugin` comme par l'installateur.

**La vélocité et les accords puissants sur le clavier OBS (9 novembre 2026).** Mettre en avant la force du jeu, et faire des gros accords des événements. Deux volets proposés, faits ensemble.
  - *Une échelle à la mesure du clavier.* Le B2 dépasse rarement 112 : rapportée à 127, une frappe fortissimo n'avait jamais l'air pleine. La force d'une frappe se mesure donc contre la vélocité la plus forte du clavier (112 par défaut). Un bouton prend la frappe la plus forte de la dernière minute : on joue quelques notes aussi fort que possible, puis on appuie.
  - *L'accent* (70 % par défaut) règle à quel point la force se voit :
    - une note douce est plus fine, plus profonde et plus transparente ; une forte, plus large, avec un cœur chauffé à blanc et un halo plus large ;
    - à la frappe, une onde de lumière en ellipse part de la touche, d'autant plus large et vive que la frappe est forte ;
    - l'éclat des touches suit la même échelle.
  - *Les accords puissants* : au moins 3 notes frappées en 35 ms, leur force moyenne au-dessus d'un seuil (75 % de la plus forte, soit une vélocité moyenne de 84 sur un B2).
    - L'accord n'est connu qu'une fois sa fenêtre passée, toutes ses notes arrivées. Annoncé dès sa troisième note, le feu d'artifice partait avec les seules premières notes, c'est-à-dire une main. Les 35 ms d'attente ne se voient pas.
    - Après un accord, le suivant attend 0,6 s : un passage fortissimo ne tire pas un feu d'artifice à chaque accord.
    - Les effets, chacun à cocher : une onde de lumière qui court le long des touches et du rail ; l'espace au-dessus des touches éclairé aux couleurs de chaque main ; un feu d'artifice (une fusée par main, une de plus pour six notes ou plus, qui éclate vers la moitié de la hauteur) ; les notes de l'accord reliées en haut par une barre de lumière ; et, désactivée par défaut, une secousse du clavier de quelques pixels. Leur intensité dépend du réglage, de l'écart au seuil et du nombre de notes.
  - Les réglages sont rangés dans deux groupes des propriétés : *Vélocité*, et *Accords puissants* (un groupe à cocher).
  - *Tests* : le détecteur d'accords (connu à la fin de sa fenêtre et une seule fois ; rien pour un accord doux, un arpège ou deux notes ; repos entre deux) et les fusées (montée, gerbe dans toutes les directions, rayon, retombée) dans le test sans OBS. Dans libobs : une note douce plus fine et plus terne qu'une forte ; un accord fort qui éclaire l'espace et lance un feu d'artifice, rien pour un accord doux ni sans l'option ; images d'un gros accord à deux mains.

**Le clavier OBS : la durée du son, la mélodie, les lumières et les styles (10 novembre 2026).** Huit suggestions retenues sur douze.
  - *Les notes tenues par la pédale.* Une barre s'arrêtait au relâchement de la touche, même quand la pédale forte faisait encore sonner la note. L'état du clavier (`KeyboardState`) garde maintenant la fin du son de chaque note. C'est le relâchement de la touche, ou plus tard le lever de la pédale forte, celui de la tonale (pour les touches qu'elle tenait en s'abaissant), ou la touche refrappée. Sous la barre, une traîne plus étroite et plus pâle dure tant que la note sonne. Une ligne de lumière court le long du clavier tant que la pédale forte est enfoncée.
  - *Les notes s'éteignent.* Le long de sa barre, une note faiblit comme une corde : son éclat suit 0,3 + 0,7·e^(−t/τ), avec τ de 7 s dans les graves à 1,2 s dans les aigus. Le dégradé de la barre va de l'éclat de la frappe à celui du relâchement, et la traîne continue la descente.
  - *La ligne mélodique* : la note du dessus de chaque groupe joué par la main droite (35 ms), reliée aux suivantes par une courbe de Catmull-Rom. Le shader la trace d'un seul dessin, en mesurant la distance à une polyligne de 64 points au plus passée en tableau uniforme. Un dessin par segment aurait doublé la lumière aux jointures.
  - *Le bloom.* Le clavier est d'abord dessiné dans une image, en couleur prémultipliée. Ses parties vives et colorées (la saturation écarte les touches blanches) sont prises au quart de la taille, floutées deux fois dans chaque direction (9 échantillons), puis ajoutées par-dessus. Le dessin passe alors par trois images intermédiaires, pour environ 1 ms par image chargée.
  - *Le reflet* : la bande au-dessus des touches devient une laque noire, de 17 % de la hauteur du clavier au lieu de 5 %. Elle reflète les notes (en miroir, écrasées à 55 %, de plus en plus pâles avec la profondeur) et les lumières des touches. Le dessin y est découpé à la bande.
  - *L'ambiance.* Le plugin lit le mix « tel qu'entendu » sur les 1 024 dernières trames montrées, et le niveau en dB donne une force de 0 à 1 (−48 à −14 dBFS), qui monte vite et redescend lentement. Des voiles de lumière ondulent au-dessus des touches, de la couleur de gauche à celle de droite.
  - *Les styles* : PianoForge (les réglages par défaut), Concert, Néon, Galaxie, Arc-en-ciel, Minimal.
    - Un style remet le rendu par défaut puis pose ses propres valeurs. Il ne touche ni à la taille, ni aux touches affichées, ni au minutage, ni à l'échelle de vélocité.
    - Il n'est appliqué qu'une fois, quand on le choisit : OBS rappelle les fonctions de modification à chaque ouverture des propriétés, et réappliquer le style écraserait les retouches faites depuis.
    - Un bouton remet tous les réglages par défaut, sauf la largeur.
    - Les réglages sont rangés en groupes : Clavier, Couleurs, Notes, Lumières, Paillettes, Vélocité, Accords puissants.
  - *Après l'essai, les traînées et la ligne de la pédale.*
    - Les traînées ont trois réglages : largeur (35 % de la note), opacité (50 %) et fin effilée (80 %).
    - Une fois la pédale levée, la fin de la traînée s'amincit et s'estompe. Il a fallu ajouter au shader une boîte qui se resserre vers le bas (`box_taper`) : la demi-largeur diminue avec la hauteur, et l'arrondi suit.
    - Tant que la note sonne, la traînée descend jusqu'aux touches, à peine resserrée.
    - Le trait orange au-dessus du clavier, pédale enfoncée, n'était pas réglable. Il a maintenant son propre curseur (30 % par défaut, 0 pour aucun), indépendant des traînées.
    - Dans la laque, chaque traînée se reflétait à la largeur de sa note, en pavé. La barre et la traînée s'y reflètent maintenant chacune à sa largeur.
  - *Les touches affichées.*
    - Au choix : 88, 76, 61, 49 ou 37 touches, une plage d'une touche à une autre, ou le suivi du jeu.
    - La géométrie passe par une vue : la première touche blanche montrée et le nombre de touches blanches en travers. Les touches gardent les proportions d'un piano pour leur largeur.
    - En suivant le jeu, la vue couvre les touches jouées dans les 4 dernières secondes, tenues ou qui sonnent encore, avec une marge, sur au moins 3 octaves.
    - Elle ne bouge que si une note sort de ses bords ou si elle peut rétrécir d'un quart, puis glisse en 0,6 s. Une vue qui bouge à chaque note fatigue.
  - *Tests* :
    - sans OBS : la fin du son (pédale forte, tonale, refrappe, oubli des notes) ; la vue (plage, bords noirs, suivi, immobilité à l'intérieur) ; la mélodie ;
    - dans libobs : une traîne sous la note tenue, absente sans l'option et finie au lever de la pédale ; la ligne mélodique entre deux notes ; 61 touches (do2 au bord gauche, clavier plus haut) et le suivi ; le bloom autour d'une note éclairée, sans toucher les touches blanches ; le reflet de la teinte dans la laque ; l'ambiance pendant que le son joue ; les styles appliqués une seule fois et la remise à zéro ; des images du rendu par défaut ;
    - les tests ciblés coupent bloom, reflet et ambiance pour rester isolés.

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
