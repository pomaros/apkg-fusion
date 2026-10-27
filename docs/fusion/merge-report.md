<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# Bilan du merge APKG

Document de clôture en préparation. Le produit hors portail n'est pas encore
déclaré terminé. Les résultats mesurés et les compteurs sont dans
[status](status.md) ; les points à fermer avant le prochain essai M1 sont dans
la [liste de clôture](non-portal-checklist.md). La séquence s'arrête ensuite,
avec remise de ce bilan. Le portail attend une nouvelle instruction.

## Ce qui est conservé et ce qui change

La fusion reprend le modèle d'APKG : un contexte explicite par racine, des plans,
un journal de récupération, un CLI et PkgManager avec son worker DOS/ARexx.
APKG avait déjà ces capacités, les dépendances, les révisions, les préversions,
des vues récentes et des indications de revue/test. Elles ne doivent pas être
présentées comme des inventions du merge. Le
[README importé](../../components/arospkg/README.md) décrit ce point de départ.

Le moteur livré par défaut est une implémentation commune dans `src/apkg`,
construite autour de ce modèle et des services portables de pkg. Il ne fait pas
alterner deux gestionnaires selon l'opération. Les moteurs d'origine sont gardés
comme références de comportement et suites de régression. Les détails et les
limites sont dans l'[architecture](architecture.md).

| Sujet | Différence apportée à APKG | État et limite |
|---|---|---|
| Moteur et clients | CLI et worker PkgManager appellent la même API commune, avec résultats typés et plans partagés. Le backend provisoire à état global est exclu du produit par défaut. | Contrôles de symboles, routage et équivalence sur hôte ; clôture complète des contrôles de la fenêtre encore à vérifier. |
| Utilisation depuis les hôtes | Le CLI, la bibliothèque, l'empaquetage et la publication communs compilent et travaillent sur macOS/Linux. | Tests réels sur hôte ; cela ne signifie pas que les programmes AROS s'exécutent sur macOS, ni que Windows est qualifié. |
| Identité d'un paquet | Autorité, nom, version, révision, cible et contenu exact sont distincts. Deux auteurs peuvent publier le même nom sans fusion de leurs identités. | Un choix ambigu refuse ; les interfaces gardent les valeurs complètes. |
| Cibles | CPU, OS, ABI et environnement d'exécution voyagent ensemble dans la description signée. | Une compatibilité de métadonnées ne constitue pas un test d'exécution. |
| Signatures et confiance | Descripteurs, catalogues et attestations utilisent des signatures séparées ; les rôles auteur, canal, réviseur et testeur sont indépendants. | Politique explicite par racine. Une signature valide ne vaut ni confiance automatique, ni revue, ni validation matérielle. |
| Publication ouverte | Un auteur peut produire son propre canal signé local ou HTTP, consommable directement sans catalogue central ni revue préalable. | Flux d'empaquetage/publication testé ; adaptation complète du portail hors séquence actuelle. |
| Source d'auteur | Le TOML et les métadonnées représentées alimentent le descripteur commun, le bundle, l'archive et les signatures. | Les entrées restent intactes ; une sémantique non représentée refuse explicitement. |
| Transport | Les archives et objets sont transférés par morceaux bornés, hachés et liés à la sélection exacte avant installation. | Cas de corruption, troncature, cache altéré et interruption testés ; transport réseau AROS encore à qualifier. |
| Sources multiples | La racine possède une liste ordonnée de sources ; sélection et dépendances passent par le résolveur commun. | Pas de choix silencieux entre deux autorités portant le même nom, ni d'omission silencieuse d'une source indisponible. |
| Protection contre le rejeu | L'acceptation explicite retient une séquence et un digest de catalogue dans le journal de la racine. | Un retour arrière de paquet ne fait pas redescendre ce seuil ; la fraîcheur en ligne reste une question distincte. |
| Confirmation | Le plan approuvé désigne le contenu, la racine, la politique et les sources observées. Les changements de préconditions invalident l'approbation. | La confirmation ne relance pas une résolution différente. |
| Revue et tests indépendants | Des attestations signées s'appliquent au descripteur exact et à leur propre périmètre. Les observations peuvent être conservées hors ligne avec leur provenance. | Un paquet modifié n'hérite pas de la qualification ; aucun auteur ne s'auto-attribue un rôle de réviseur. |
| Configuration et propriété | Installation, adoption, mise à jour, réparation et suppression utilisent les mêmes règles pour les fichiers livrés, les éditions personnelles et les chemins non possédés. | Tests de conservation et refus de prise de possession ; qualification native des métadonnées étendues encore ouverte. |
| Retour arrière et réparation | Le moteur retient du contenu vérifié et réutilise le même journal pour les opérations hors ligne. | Les anciens historiques APKG conservés comme preuve ne deviennent pas automatiquement des retours arrière communs utilisables. |
| Inspection | Les clients exposent séparément contenu déclaré, signature, confiance, cible, état installé et vérification des fichiers vivants. | Les faits inconnus ne sont pas transformés en valeurs vérifiées. |
| PkgManager/ARexx | Catalogue signé, valeurs complètes, sélections exactes, compteurs de progression et confirmation passent par les adaptateurs communs. | Des contrôles hérités sont encore ignorés, les anciennes actions rapides Upgrade/Rollback sont désactivées et les vues récentes restent à raccorder. Le formulaire commun fournit les opérations moteur ; cela ne termine pas l'interface héritée. |
| Run et bundles | L'inspection autonome d'un bundle ne demande pas d'enregistrement. Le raccord de Run géré doit retrouver son application déclarée, y compris dans un sous-dossier. | Correction et tests de préparation sur hôte ; essai du candidat complet sur M1 restant. Les choix multiples refusent. |
| Auto-mise à jour | Le client géré dans une racine passe par le plan et le journal communs. | Les copies réparties entre plusieurs volumes restent une obligation plus large. L'updater installé du M1 n'est pas migré ici. |
| Migration | Préparation d'une copie, inventaires, associations explicites avec des artefacts signés, approbation exacte, import groupé et reprise. | Les originaux restent utilisables. Profil et fixture épinglés testés ; conservation de paramètres/lancement/historique ne vaut pas conversion active ni migration de distribution réelle. |

## Changements à prendre en compte par les intégrations

Le stockage commun sous `.apkg` et l'API `apkg_*` ne sont pas un remplacement
binaire transparent de l'ancienne bibliothèque et de son registre. L'adaptateur
de migration prépare une copie ; il ne convertit pas silencieusement une
installation active. La migration du consommateur `libpkg` qui met le M1 à jour
appartient à son propriétaire et reste séparée de ces essais.

Le [profil de commandes historiques](legacy-commands.md) décrit les commandes,
sorties et codes effectivement adaptés. Il ne promet pas que tous les anciens
scripts sont compatibles. Les variantes encore refusées, les conversions de
paramètres/lancement/historique et la continuité des catalogues doivent rester
visibles dans les obligations de migration.

La confiance et les sources doivent être configurées explicitement. Importer
une installation ou ajouter une source ne transforme pas les anciens fichiers
en contenu authentifié rétroactivement et n'ajoute pas de clés à l'insu de
l'opérateur.

## Validation et clôture

Les tests hôtes couvrent les opérations réelles sur des racines temporaires,
les vrais paquets/signatures/canaux, les refus et la récupération ; ce ne sont
pas seulement des contrôles de compilation. Les simulations de panne de
processus et les sanitizers ne prouvent pas la résistance à une coupure de
courant.

Un [premier profil M1 isolé](evidence/m1-ram/README.md) a exercé des opérations
CLI et GUI sur RAM, avec des corrections d'identité de fichiers, de création
du cache et d'observation des permissions DOS. Ce résultat conserve ses limites
de composants et de runtime. Il ne valide pas les changements ultérieurs ni
la fusion complète. Le prochain candidat doit passer les étapes hôtes puis
l'essai M1 prévu avant remise du bilan final.

La clôture devra identifier le commit, les binaires, les résultats CI et M1,
les adaptations restantes et les limites de publication. Aucun déploiement du
portail, accord de mainteneur, migration réelle de distribution ou lancement
commun aux deux projets ne doit être déduit de ce document.
