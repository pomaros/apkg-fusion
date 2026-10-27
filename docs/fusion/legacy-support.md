<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# Compatibilité legacy : choix et travaux différés

Les lots CLI-CONTINUITY (3), IMPORT-PROFILES (4) et PRIOR-VERSIONS (5) sont
différés sur instruction de l'opérateur. Leur achèvement ne conditionne plus
la validation du candidat courant sur macOS/Linux puis M1, ni le bilan avant
le portail. Ce report ne vaut pas validation d'une migration d'installation
existante. Les adaptateurs et tests déjà présents restent conservés.

Le [statut](status.md) porte les résultats ; le
[plan](../../PLAN.md#remaining-closure-batches) porte la séquence active.
Les tailles ci-dessous sont relatives, sans engagement de délai.

## Choisir le support promis

Ces profils sont combinables : reprendre des installations n'impose pas de
reproduire toutes les commandes historiques. Conserver une archive ne la rend
pas automatiquement utilisable pour restaurer une version.

| Support choisi | Promesse et limite | Travail supplémentaire | Taille indicative |
|---|---|---|---|
| Installation neuve uniquement | Moteur, clients et formats communs sur une racine neuve ; aucune reprise automatique de racines ou scripts historiques. | Documenter l'installation séparée et les refus ; conserver les tests des adaptateurs existants. | S de documentation, qualification dans les lots actifs |
| Conservation et consultation | Garder les originaux et leurs faits, y compris les inconnues ; aucune activation implicite des paramètres ou historiques. | Définir les formats admis, inventaire/export et diagnostics ; vérifier copies, permissions, absences et interruptions sur des captures réelles. | M, selon les formats |
| Reprise des installations | Retrouver les applications, éditions personnelles, placements, paramètres et lancements dans le modèle commun. | IMPORT-PROFILES ci-dessous ; scripts anciens à sélectionner séparément. | L |
| Continuité des commandes et scripts | Ensemble nommé de commandes, options, sorties JSON et codes de retour historiques. | CLI-CONTINUITY pour le corpus choisi ; aucune compatibilité universelle implicite. | L pour le périmètre précédemment prévu |
| Reprise des anciennes versions | Restaurer les versions dont l'identité et les octets sont disponibles et admissibles. | PRIOR-VERSIONS ; déclarer les restaurations impossibles. | L–XL, incertitude la plus forte |

## CLI-CONTINUITY — commandes et scripts (lot 3)

Le [profil existant](legacy-commands.md) couvre un sous-ensemble natif de
list/info/verify/install/upgrade/remove/doctor et des aperçus. Il ne garantit
pas tous les scripts d'une ancienne distribution.

À sélectionner puis réaliser selon les consommateurs réellement retenus :

- update/search/recent/outdated, mises à jour groupées et rollback par nom :
  définir leur correspondance avec les sélections et plans communs.
- Catalogues `--index`/`--index-url`, arguments de lancement et options
  d'auto-mise à jour. Une adaptation de syntaxe ne peut authentifier un ancien
  catalogue non signé.
- Champs JSON, valeurs absentes, sorties texte, codes de retour et confirmation
  attendus par les scripts sélectionnés ; documenter les changements imposés.
- Options d'injection/rapport réservées aux tests : décider de les exclure ou
  de les fournir dans un outil de test plutôt que de les promettre implicitement.

Sortie du lot : corpus versionné de scripts, succès et refus sur copie importée,
effets équivalents aux plans communs et aucun argument silencieusement ignoré.
Compléter les essais sur les environnements effectivement revendiqués.

## IMPORT-PROFILES — installations, paramètres et lancement (lot 4)

La [copie existante](legacy-copy.md) importe un profil schema-1 avec relations
et exigences système sous approbation explicite. Des octets de paramétrage ou
de lancement conservés ne sont pas une configuration active convertie.
La [cartographie](migration.md) décrit les données et leurs incertitudes.

- Fixer versions sources, schémas et variantes admis ; recueillir des captures
  originales autorisées avec un inventaire reproductible.
- Compléter les placements : chemins partagés, icônes, propriétaires,
  collisions, fichiers modifiés/inconnus et attributs du système de fichiers.
- Convertir paramètres, associations ENV/ENVARC, racines et sources avec leur
  portée. Ne pas déduire une confiance ou qualification d'une préférence client.
- Réconcilier métadonnées de lancement et `application.json` ; valider chemins
  et désaccords sans exécuter de programme pendant l'import.
- Éprouver validation, copie, interruption, reprise et répétition ; garder
  l'installation source utilisable et refuser les cas non admis.

Sortie du lot : rapport source → état commun, conservation vérifiée des
originaux, puis diagnostic, lancement, mise à jour, réparation et suppression
de la copie. Inspecter fichiers, données personnelles et journal ; compléter
par les essais natifs nécessaires aux attributs DOS.

## PRIOR-VERSIONS — historiques et restauration (lot 5)

Le rollback de versions créées par le moteur commun reste dans les tests
actifs. Seule la conversion de l'ancien historique est différée ici.

- Inventorier registres précédents, archives, références et journaux
  d'auto-mise à jour : disponible, absent, non résolu et non vérifié sont distincts.
- Définir identité, version/révision, cible, provenance, admissibilité et
  propriété pour chaque version reprise. Des octets retenus ne gagnent aucune
  signature ou autorisation rétrospective.
- Construire les entrées utilisables par les plans et le journal communs ;
  préserver séparément les preuves non convertibles.
- Définir rétention/nettoyage et traitement des anciens journaux non terminés,
  sans exécuter deux modèles de récupération sur la même mutation.

Sortie du lot : restauration hors ligne des versions déclarées disponibles,
refus motivé des autres, interruption/reprise, contrôle des éditions personnelles
et de l'état enregistré. Une archive absente interdit de promettre sa restauration.

## Informations nécessaires à la reprise

La seconde distribution, les versions APKG déployées, les versions de
`dos.library`, les gestionnaires de fichiers et les scripts réels ne sont pas
établis. La cartographie cite AROS One 1.3 à partir de documents amont ; ce n'est
pas une capture ni une qualification de cette distribution.

Avant toute promesse, nommer le profil, ses versions et son corpus, collecter
ces données et définir les conditions de refus/repli. L'accord des mainteneurs
et la qualification des distributions restent des conditions de diffusion
commune ; les tests hôtes ne les établissent pas.

## Séparations à conserver

La migration entre versions de l'outil APKG lui-même (exécutable, bibliothèque,
formats persistants, clients, sauvegarde et retour arrière) fait l'objet d'une
revue distincte dans [TODO](../../TODO.md). Un import d'applications ne la démontre
pas. La migration du consommateur `libpkg` qui met le M1 à jour garde son
propriétaire séparé. Le portail reste après la frontière d'arrêt demandée.
