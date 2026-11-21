# Stati operativi su `J30001`

La cattura fornita dal proprietario dell'Ungaro con Tiemme SY400 mostra
`J30001 = 1` (Check Up) dopo Start e la richiesta `AT+CMGD=1` con
due NUL prima del CR. Sulla stessa RS232 funzionavano Start, Stop e le
normali letture 4Heat, con `A50 = 1` come indicato dal manuale della
caldaia. La cattura fornita copre il codice 1. Per gli altri codici la
tabella distingue le osservazioni pubbliche da quanto riporta l'esempio
originale.

| Codice | Etichetta originale | Etichetta nell'esempio italiano | Evidenza |
| --- | --- | --- | --- |
| 0 | OFF | Spento | Cattura 4Heat pubblica |
| 1 | Check Up | Check Up | Cattura RS232 dell'Ungaro dopo Start |
| 2 | Ignition | Accensione | Esempio originale |
| 3 | Stabilization | Stabilizzazione | Osservazione 4Heat pubblica |
| 4 | Ignition | Accensione | Esempio originale |
| 5 | Run | Normale | Osservazione 4Heat pubblica |
| 6 | Modulation | Modulazione | Osservazioni 4Heat pubbliche |
| 7 | Extinguishing | Spegnimento | Cattura 4Heat pubblica |
| 8 | Safety | Sicurezza | Identificazione pubblica dall'app 4Heat, da verificare sul campo |
| 9 | Block | Blocco | Osservazione 4Heat pubblica |
| 10 | Recover Ignition | Recupero accensione | Osservazioni 4Heat pubbliche, direzione coerente col manuale SY400 |
| 11 | Standby | Standby | Osservazioni 4Heat pubbliche, semantica coerente col manuale SY400 |
| 30 | Ignition | Accensione | Osservazione 4Heat pubblica |
| 31 | Ignition | Accensione | Osservato come Check Up su un altro 4Heat, comunque fase di avvio |
| 32 | Ignition | Accensione | Mostrato come Ignition nell'app nella prova pubblica, sottofase da verificare |
| 33 | Ignition | Accensione | Presente nel template pubblico e nell'esempio originale, sottofase da verificare |
| 34 | Ignition | Accensione | Esempio originale, sottofase da verificare |

La tabella si basa sulle seguenti fonti. [esempio del repository originale](https://raw.githubusercontent.com/leoshusar/4heat-esphome/main/example.yaml),
[catture e osservazioni 4Heat](https://community.home-assistant.io/t/tiemme-4heat-stove-wifi-controller/374040/5),
[ulteriore osservazione del codice 10](https://community.home-assistant.io/t/tiemme-4heat-stove-wifi-controller/374040),
[conferme pubbliche di modulazione e standby](https://community.home-assistant.io/t/tiemme-4heat-stove-wifi-controller/374040?page=2).

## Semantica operativa

Il manuale SY400 STD01 Combi descrive il recupero accensione come una
fase che può seguire un'interruzione dell'alimentazione da uno stato ON
(§8.1.5). Lo Standby può spegnere temporaneamente la fiamma o mantenere
il braciere e il sistema può riprendere automaticamente al rientro delle
condizioni (§8.1.8 e §10.3). Il componente rappresenta entrambi come ON.
La descrizione di `Stop` nel capitolo sul modem (§10.1) è generale e
lascia da verificare l'accettazione del comando nelle singole fasi sulla
revisione Ungaro. Per questo Stop è ammesso in Normale e Modulazione.
[Tiemme SY400 STD01 Combi](https://www.semsystem.it/_files/ugd/c0d129_d4dbaa1b1fb04e51a988e19aa8b00d8c.pdf)

La numerazione dei parametri dipende dal firmware. Per l'Ungaro vale il
requisito `A50 = 1` riportato dal proprietario nel suo manuale. Il
manuale Mescoli per SY400 Modbus Essenziale indica `P90 = 1` al §11.23
per quel firmware. [Mescoli SY400 Modbus Essenziale](https://www.mescolicaldaie.it/wp-content/uploads/2023/12/Man_Ass_SY400_Modbus-Essenziale-V0.3-V0.4-0519_it.pdf)

## Potenze visualizzate sul display Ungaro

Le sigle citate per la pagina 90 del manuale installatore Ungaro del 2013
sono `PA1` (Accensione), `PB` (Stabilizzazione), `P1`–`P7` (Normale
o Modulazione al rispettivo livello), `PM` (Modulazione ridotta), `PE`
(Standby) e `PS` (Spegnimento). La copia di quella pagina non era
disponibile. La stessa legenda per il campo della potenza è presente
nel §4.1 del manuale utente Ungaro Fluenth CTU. Le fonti consultate
non pubblicano una corrispondenza fra queste sigle e i valori numerici
4Heat. [Manuale utente Ungaro Fluenth CTU, §4.1](https://it.scribd.com/document/797750673/Manuale-Uso-Caldaia-Pellet-Ungaro-CTU-L)

## Verifica sul dispositivo

Il ramo aggiornato è stato verificato con simulazione RS232 e compilazione
ESP32. Restano da verificare sull'Ungaro la temporizzazione effettiva di
`AT+CMGD` e le sequenze complete di `J30001` durante accensione,
spegnimento, Standby e recupero. Una risposta 4Heat tardiva con lo stesso
ID durante una nuova lettura di quel datapoint è indistinguibile dalla
risposta attesa a livello di frame.
