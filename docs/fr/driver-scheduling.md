# Ordonnancement préemptif des pilotes Windows

L’objet facultatif `scheduling` active une préemption déterministe sur CPU0 pour les pilotes Windows x64 avec Unicorn, KVM ou WHP. Son omission conserve le mode coopératif et le temps virtuel avançant uniquement au repos. Les deux champs sont des entiers positifs dont le produit tient dans `uint64_t`. Un objet vide sélectionne les valeurs ci-dessous.

```json
{
  "scheduling": {
    "quantum_instructions": 1024,
    "instruction_time_100ns": 1
  }
}
```

`quantum_instructions` limite les tentatives d’instructions machine admises pour un thread. `instruction_time_100ns` attribue une durée virtuelle à chaque tentative, sans estimer la vitesse matérielle. Les API du modèle et les transactions d’instructions se terminent avant tout changement. Une échéance ne réinitialise pas le quantum restant. Le rapport conserve la politique dans `configuration.scheduling`.

`KeSetPriorityThread` et `KeQueryPriorityThread` accèdent à la priorité courante à `PASSIVE_LEVEL`. Le réglage accepte 1..31 et renvoie la valeur précédente ; zéro est réservé. Le profil déterministe initialise chaque thread à 8. Le thread prêt de plus haute priorité est choisi ; les égalités suivent l’ordre de disponibilité et la rotation des quantums. Un réveil ou un changement de priorité supérieure préempte avant l’instruction invitée suivante sous DISPATCH_LEVEL, en conservant le quantum restant. Les callbacks imbriqués et SEH partagent la priorité du thread. Un objet système terminé mais encore référencé reste consultable jusqu’à son retrait ; sa modification est refusée. Les objets empruntés des threads de callback expirent avec leur propriétaire, sans transmettre l’ancienne priorité lors du réemploi de la pile.

Les continuations prêtes, nouveaux workers et threads système partagent un ordre de disponibilité. Appels imbriqués et SEH partagent le quantum de leur thread. Le changement préserve le contexte CPU complet, l’identité du thread, l’état APC, l’IRQL effectif et les mappings du processus. PASSIVE/APC autorise la préemption ; DISPATCH et au-delà masquent le changement de thread. Les régions critiques et protégées désactivent les APC, sans interdire la préemption.

Les instructions font avancer les timers et le DMA par échéances chronologiques. L’annulation attend la disponibilité du verrou cancel et, pour WDM, le retour de dispatch. Les complétions provider paginables, la politique d’alimentation et PoFx attendent une frontière PASSIVE ; les échéances restent en attente et les observations indiquent l’heure réelle du service. Les callbacks indépendants produits par l’horloge restent distincts des callbacks PoFx bloquants du thread appelant. Le résultat d’une attente est fixé à son échéance, avant une réinitialisation de timer ou un signal ultérieur, même si une tentative d’instruction franchit les deux échéances.

Les classes et ajustements dynamiques de priorité Windows, les CPU parallèles du modèle OS, l’imbrication arbitraire des interruptions, la livraison d’APC et les threads Windows ring3 ne sont pas implémentés. L’exécution parallèle CPU est une capacité distincte. Des pilotes originaux compilés vérifient les quanta courts, les états flottants/GS, les attentes temporisées, les attachements de processus entrelacés, l’annulation et les continuations PoFx. Les preuves natives ARM64 restent suivies séparément.
