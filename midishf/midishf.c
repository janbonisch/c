/*
to compile use this command:

gcc midishf.c -lasound -lpthread -o midishf
 
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>  //linker opt. -lpthread
#include <alsa/asoundlib.h>     //Interface to the ALSA system, linker opt. -lasound

//rizeni ukecanosti
uint16_t verbose;
#define VERBOSE_RAW_MIDI         0x01
#define VERBOSE_MIDI_EVENT       0x02
#define VERBOSE_READ_FILE        0x04
#define VERBOSE_SHOW_CFG         0x08
#define VERBOSE_PRESET_CHANGE    0x10
#define VERBOSE_COMMAND          0x20
#define VERBOSE_MIDISH           0x40

const char prog_name[] = "midish frontend";
const char prog_ver[] = "beta 0.1";

void error(const char *format, ...) {
  va_list ap;
  va_start(ap, format);
  vfprintf(stderr, format, ap);
  va_end(ap);
  putc('\n', stderr);
}

//------------------------------------------------------------------------------
//Neblokujici cteni klaviatury
//Uz jsem nevedel, jak na to, na netu mimo jine rada s extra vlaknem. Nakonec
//teda jo. A dlabu na nejaky mutexy, bo zapisovaci ukazovatko meni jen vlakno
//a cteci zase jen ten, kdo vyzvedava kod, takze k hazardu nedojde (snad).

#ifndef KBD_BUFFER_P2   //pokud neni definovana velikost bufajze, tak default
#define KBD_BUFFER_P2 4 //mocnina dvojky pro velikost bufiku
#endif
#define KBD_BUFPC(x) ((x+1)&((1<<KBD_BUFFER_P2)-1)) //zakruhovani ukazovatka podle velikosti bufiku daneho mocninou dvojky

typedef struct {
  int write; //zapisovaci ukazovadlo
  int read; //cteci ukazovadlo
  char buffer[1 << KBD_BUFFER_P2]; //bufik  
  pthread_t threadDescriptor;
} KBD_CONTEXT;

KBD_CONTEXT kbd_context; //stav klaviatury

void* kbd_thread(void *vargp) {
  int i;

  while (kbd_context.threadDescriptor != 0) { //jedem s medem a furt dokola, pripadne to lze zabit vynulovanim deskriptoru, ale kdo by se s tim matlal, exit vyresi
    i = kbd_context.write; //vezmeme pozici
    kbd_context.buffer[i] = getchar(); //nabereme znak
    kbd_context.write = KBD_BUFPC(i); //zakruhujeme ukazovatko
  }
  pthread_exit(NULL);
}

///Neblokující čtení klávesnice.
///@return 0=nemáme nic nového / ostatní znak z klávesnice

char kbd_getc(void) {
  int i;
  char c;

  i = kbd_context.read; //beru cteci ukazovadlo
  if (i == kbd_context.write) return (char) 0; //pokud nic noveho, tak proste nic
  c = kbd_context.buffer[i]; //vezmeme znak
  kbd_context.read = KBD_BUFPC(i); //zakruhujeme ukazovatko
  return c; //zvracim kod znaku
}

///Inicializace neblokujicího čtení klávesnice.

void kbd_init(void) {
  memset(&kbd_context, 0, sizeof (kbd_context)); //nulujeme nulama
  pthread_create(&kbd_context.threadDescriptor, NULL, kbd_thread, NULL); //startujeme vlakno na klavesnici, bo jinak nevim jak udelat neblokujici cteni  
}

//Zakončení čínnosti neblokujícího čtení klávesnice

void kbd_done(void) {
  kbd_context.threadDescriptor = 0; //tohle zabije vlakno s klavesnici
}

//------------------------------------------------------------------------------
// Spoluprace s MIDISH

typedef struct CFG_LINE CFG_LINE;

struct CFG_LINE {
  CFG_LINE* next; //ukazatel na dalsi radek, NULL je koncovka
  char line; //tady je prvni znak radku
};

typedef struct CFG_PRESET CFG_PRESET;

struct CFG_PRESET {
  char* name; //jmeno predvolby
  int param; //hlavni parametr, pro main je to kontroler, pro predvolbu je to cislo noty  
  CFG_PRESET* next; //ukazatel na dalsi predvolbu, NULL je koncovka
  CFG_LINE* midishcmd; //ukazatel na prvni radek
};

CFG_PRESET preset; //default predvolba
FILE *midish; //rukovet k procesu ve kterym bezi MIDISH

//spusteni MIDISH

void start_midish(void) {
  midish = popen(((verbose & VERBOSE_MIDISH) != 0) ? "midish -v" : "midish", "w"); //otvirame proces
  if (midish == NULL) { //pokud to nedopadlo
    printf("midish start error!\n"); //vyzvracime hlaseni
    exit(EXIT_FAILURE); //a slus
  }
}

//ukonceni MIDISH

void stop_midish(void) {
  pclose(midish);
}

//Nastavy zvoleny preset

void midish_set_preset(CFG_PRESET* pp) {
  CFG_LINE* l;

  if ((verbose & VERBOSE_PRESET_CHANGE) != 0) printf("Set preset '%s'\n", pp->name);
  l = pp->midishcmd;
  while (l != NULL) {
    if ((verbose & VERBOSE_COMMAND) != 0) printf("%s\n", &l->line);
    fprintf(midish, "%s\n", &l->line);
    fflush(midish);
    l = l->next;
  }
}

//------------------------------------------------------------------------------
// Prace s midi udalostmi

uint8_t trigger; //stav ovladaciho kontrolerru, nenula zmanema, ze probiha cekani na notu
uint8_t learned_param; //naucena hodnota behem aktivniho ovladaciho kontroletu
uint8_t event_trigger; //co nam ovlada spoust
uint8_t event_param; //z ceho berem parametr

void midi_proc_event(uint8_t cmd, uint8_t data1, uint8_t data2) {
  CFG_PRESET* pp;

  if ((verbose & VERBOSE_MIDI_EVENT) != 0) printf("event %02X %d %d\n", (int) cmd, (int) data1, (int) data2);
  if (cmd == event_trigger) { //pokud je to kontroler na spravnem kanale
    if (data1 == preset.param) { //pokud je to kontroler, ktery ridi prepnuti, tak hura
      if (data2 < 0x40) { //kontroler vypnut
        if (trigger != 0) { //pokud jeste nebyl vypnut          
          trigger = 0; //tak ted vypnut teda jako ze bude          
          if (learned_param != 0) { //pokud tam mame alespon neco
            pp = preset.next; //tady zacina prvni preset
            while (pp != NULL) {
              if (pp->param == learned_param) {
                midish_set_preset(pp);
              }
              pp = pp->next;
            }
            learned_param = 0; //a nemame
          }
        }
      } else { //kontroler zapnut
        if (trigger == 0) { //pokud jeste nebyl zapnut
          trigger = -1; //odted budem povazovat za zapnuty
          midish_set_preset(&preset); //a rovnou aktivujeme default, kterej ma za ukol hlavne vypnout routovani, aby to nebrnklo                    
          learned_param = 0; //a nemame
          //TODO: mozna nejakej timeout, aby to pripadne nezamrzlo navzdy (vytrzeni kabelu)
        }
      }
    }
  } else if (cmd == event_param) { //pokud je to noteon na spravnem kanale
    if (trigger != 0) { //pokud jsme ve fazi prijmu klapky s predvolbou
      learned_param = data1; //schovame si noticku, az bude vypnut kontroler, tak ji pouzijeme
    }
  }
}

//------------------------------------------------------------------------------
// Vazba na midi system, styl RAW

uint8_t event_rx_st;
uint8_t event_buf[3];
snd_rawmidi_t* midiin = NULL;
char* portname;

void rawmidi_init(void) {
  int status;

  if ((status = snd_rawmidi_open(&midiin, NULL, portname, SND_RAWMIDI_NONBLOCK)) < 0) {
    error("Problem opening MIDI input: %s", snd_strerror(status));
    exit(1);
  }
  trigger = 0; //spoust neni aktivni
  learned_param = 0; //a pro sichr ani notu neznam
}

void rawmidi_proc(void) {
  int status;
  uint8_t data;

  status = 0;
  while (status != -EAGAIN) {
    status = snd_rawmidi_read(midiin, &data, 1);
    if ((status < 0) && (status != -EBUSY) && (status != -EAGAIN)) {
      error("Problem reading MIDI input: %s", snd_strerror(status));
    } else if (status >= 0) {
      if ((unsigned char) data >= 0x80) { // print command in hex
        if ((verbose & VERBOSE_RAW_MIDI) != 0) printf("\n0x%x ", (unsigned char) data);
        event_buf[0] = data; //tak nam zacina event
        event_rx_st = 1; //a pripravime si ukazovatko do bufiku
      } else { //data
        if ((verbose & VERBOSE_RAW_MIDI) != 0) printf("%d ", (unsigned char) data);
        if (event_rx_st<sizeof (event_buf)) event_buf[event_rx_st] = data; //ukladame pokud se vejde
        event_rx_st++; //pocitame bajtiky
        if (event_rx_st == 3) midi_proc_event(event_buf[0], event_buf[1], event_buf[2]); //pokud mame dost, tak na to muzeme reagovat
      }
    }
  }
}

//------------------------------------------------------------------------------
// Vazba na midi system, styl SEQ

static snd_seq_t *seq_handle;
static int in_port;

void seqmidi_error(int code, const char* msg) {
  char* str;
  int l;

  l = strlen(msg) + 32;
  str = alloca(l);
  snprintf(str, l, "%s (code=%d)", msg, code);
  error(str);
  exit(EXIT_FAILURE);
}

void seqmidi_init(void) {
  char str[128];
  int status;

  if ((status = snd_seq_open(&seq_handle, "default", SND_SEQ_OPEN_INPUT, 0)) != 0) seqmidi_error(status, "Could not open sequencer");
  if ((status = snd_seq_set_client_name(seq_handle, prog_name)) != 0) seqmidi_error(status, "Could not set client name");
  if ((in_port = snd_seq_create_simple_port(seq_handle, "in", SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE, SND_SEQ_PORT_TYPE_APPLICATION)) < 0) seqmidi_error(in_port, "Could not open port");
  if ((status = snd_seq_nonblock(seq_handle, 1)) != 0) seqmidi_error(status, "Could not set nonblock mode");
  snprintf(str, sizeof (str), "aconnect '%s' '%s'\n", portname, prog_name); //pripravim si prikaz pro spojeni
  if ((status = system(str)) != 0) seqmidi_error(status, "Could not connect to ctrl port"); //provedeme to    
}

void seqmidi_proc(void) {
  int status;
  snd_seq_event_t *ev = NULL;

  for (;;) {
    switch (status = snd_seq_event_input(seq_handle, &ev)) {
      case -ENOSPC: //prusvih, preteceni
        seqmidi_error(status, "snd_seq_event_input overun!");
        return;
      case -EAGAIN: //nic neprislo
        return; //slus
      default:
        if (status < 0) {
          seqmidi_error(status, "snd_seq_event_input error");
          return;
        }
        switch (ev->type) {
          case SND_SEQ_EVENT_NOTEON:
            midi_proc_event(MIDI_CMD_NOTE_ON + ev->data.note.channel, ev->data.note.note, ev->data.note.velocity);
            break;
          case SND_SEQ_EVENT_NOTEOFF:
            midi_proc_event(MIDI_CMD_NOTE_OFF + ev->data.note.channel, ev->data.note.note, ev->data.note.off_velocity);
            break;
          case SND_SEQ_EVENT_CONTROLLER:
            midi_proc_event(MIDI_CMD_CONTROL + ev->data.control.channel, ev->data.control.param, ev->data.control.value);
            break;
          default:
            if ((verbose & VERBOSE_RAW_MIDI) != 0) error("Unsupported midi event type %d\n", ev->type);
            break;
        }
        break;
    } //hura mame udalost
  }
}


//------------------------------------------------------------------------------
// Cteni konfiguracniho souboru

//preskoci bile znaky

char* skip_white(char* str) {
  while ((*str != 0)&&(*str <= 0x20)) str++;
  return str;
}

//procedura praseni parametru z ridiciho radku, testuje shodu klice a vyskyt rovnase, vraci pozici hodnoty

char* parse_proc(char* token, const char* key) {
  int l;

  l = strlen(key);
  if (strncmp(token, key, l) != 0) return NULL; //pokud to nezacina, tak slusanec
  token = strchr(token, '='); //hledam rovnase
  if (token == NULL) return NULL;
  return skip_white(++token); //preskaceme bily znaky  
}

//praseni retezcoveho parametru

void parse_str(char* token, const char* key, char** result) {
  int l;

  if ((token = parse_proc(token, key)) == NULL) return; //hledame klic
  l = strlen(token) + 1; //vysledek ma tuto delku
  *result = malloc(l); //alokujeme pamet, nezapominam na koncovou znacku
  memcpy(*result, token, l); //a perem tam vysledek
}

//praceni ciselneho parametru

void parse_int(char* token, const char* key, int* result) {
  if ((token = parse_proc(token, key)) == NULL) return; //hledame klic
  *result = atoi(token);
}

//zobrazi nastaveni predvolby

void show_preset(CFG_PRESET* p) {
  CFG_LINE* rec;

  if (p == NULL) return; //pokud nic, tak nic
  printf("*** Preset name=%s param=%d ***\n", p->name, p->param);
  rec = p->midishcmd;
  while (rec != NULL) {
    printf("%s\n", &rec->line);
    rec = rec->next;
  }
}

//zobrazeni seznamu predvoleb

void show_presets(CFG_PRESET* p) {
  while (p != NULL) {
    show_preset(p);
    p = p->next;
  }
}

//Cteni konfiguracniho souboru

void read_cfg_file(char* fname) {
  FILE* file;
  char line[1024];
  char* str;
  char* token;
  CFG_PRESET* pp;
  CFG_LINE* rec;
  CFG_LINE* wrec;
  int len;

  if ((verbose & VERBOSE_READ_FILE) != 0) printf("Read cfg file %s\n", fname);
  if ((file = fopen(fname, "r")) == NULL) { //zkusime to votevrit
    error("File open error");
    return;
  };
  preset.midishcmd = NULL; //zatim nema koniguracni radky
  preset.name = "default"; //default nema jmeno
  pp = &preset; //tady je prvni, cize default permof
  while (fgets(line, sizeof (line), file)) { //dokud dava data
    len = strlen(line); //jak je dlouhej retezec
    if (len == 0) continue; //prazdnej radek vynechame hned
    if (line[len - 1] == '\n') line[--len] = 0; //pokud je na konci novy radek, tak vyhodit
    if ((verbose & VERBOSE_READ_FILE) != 0) printf("%s\n", line);
    str = skip_white(line); //vyhodime uvodni bily znaky
    len = strlen(line); //jak je dlouhej retezec    
    if ((len == 0) || (*str == '#') || (*str == ';') || ((str[0] == '/')&&(str[1] == '/'))) { //komentare # ; // a prazdny radky ignorujeme      
      //todo: neco s tim ?
    } else if (*str == '@') { //ridici radek
      token = strtok(skip_white(++str), " "); //rozebirame podle mezery
      pp->next = malloc(sizeof (CFG_PRESET)); //udelam novy preset
      pp = pp->next; //presunu na nej kursorek
      memset(pp, 0, sizeof (CFG_PRESET)); //zacneme vynulovanim pameti, at mame vychozi hodnoty
      while (token != NULL) { //dokud mame neco
        parse_str(token, "name", &pp->name); //hledame neco znameho
        parse_int(token, "note", &pp->param); //hledame neco znameho
        token = strtok(NULL, " ");
      }
    } else { //vsechno ostatni je prikazovy radek pro midish
      len++; //potrebujeme o znak vic (koncovka ze)
      wrec = malloc(sizeof (void*)+len); //alokujeme pamet na polozku do predchoziho zaznamu      
      if (pp->midishcmd == NULL) { //pokud jeste nemame prvni radek
        pp->midishcmd = wrec;
        rec = wrec;
      } else {
        rec->next = wrec;
        rec = rec->next; //a posuneme se na novej  
      }
      memcpy(&rec->line, line, len); //kopirujeme retezec, 
      rec->next = NULL; //a zatim je to konec  
    }
  }
  fclose(file); //zavirame soubor
  if ((verbose & VERBOSE_SHOW_CFG) != 0) show_presets(&preset);
}

//------------------------------------------------------------------------------
// help

void help(void) {
  puts("\nUsage"
          "\n====="
          "\n"
          "\n-l --list        show available input ports"
          "\n-f --file        configuration file"
          "\n-p --port        midi in port for midishf"
          "\n-x --channel     midi chanel for midishf"
          "\n-c --controller  midi controller to learn key presset"
          "\n-v               max. verbose"
          "\n-verbose         verbose with mask: .0=raw .1=event .2=file .3=cfg .4=preset .5=command .6=midish"
          "\n"
          "\n"
          "\nConfiguration file"
          "\n=================="
          "\n"
          "\nConfiguration file contains three type of lines:"
          "\nline starts with @ is a preset configuration, next lines contains commands to send to midish after preset activation"
          "\nline starts with '#', ';' or '//' is a comment"
          "\nother lines are commands for midish"
          "\n(empty lines are not send to midish)"
          "\n"
          "\nexsample:"
          "\n#This (first) preset is executed after controller change to active state (value is greater than 63)"
          "\nprint \"********\""
          "\nprint \"* mute *\""
          "\nprint \"********\""
          "\ns"
          "\nfdel"
          "\nfnew midishf"
          "\n"
          "\n@name=piano note=36"
          "\n#This section is executed aftrer controller change to inactive state (value is lower than 64)"
          "\n#and before it was pressed key C2 (midi note 36)"
          "\nprint \"*********\""
          "\nprint \"* PIANO *\""
          "\nprint \"*********\""
          "\nfmap {any { 3 0} } {any { 0 3}}"
          "\ni"
          "\n"
          "\n@name=epiano note=38"
          "\n#This section is executed aftrer controller change to inactive state (value is lower than 64)"
          "\n#and before it was pressed key D2 (midi note 38)"
          "\nprint \"***********\""
          "\nprint \"* E-PIANO *\""
          "\nprint \"***********\""
          "\nfmap {any { 3 0} } {any { 0 4}}"
          "\ni"
          "\n\n");
}

//------------------------------------------------------------------------------
// SPusteni selmostroje

volatile static int ext_brk;

//Odchyt CTRL+C

void int_handler(int dummy) {
  ext_brk = -1;
}


//Start cele "aplikace"

int main(int argc, char** argv) {
  int pct, i;
  char* cfgfile;
  struct sigaction act;

  kbd_init();
  ext_brk = 0;
  act.sa_handler = int_handler;
  sigaction(SIGINT, &act, NULL);
  printf("%s     %s\n(C)2020 Jan Bonisch\n\n", prog_name, prog_ver);
  //verbose = VERBOSE_READ_FILE | VERBOSE_SHOW_CFG | VERBOSE_MIDI_EVENT | VERBOSE_PRESET_CHANGE | VERBOSE_MIDISH; //| VERBOSE_RAW_MIDI;  
  pct = 0;
  portname = NULL;
  cfgfile = NULL;
  verbose = 0;
  if (argc <= 1) {
    help();
    return EXIT_SUCCESS;
  } else while (pct < argc) {
      if ((strcmp(argv[pct], "-h") == 0) || (strcmp(argv[pct], "--help") == 0)) { //napoveda
        help(); //ukaz
        return EXIT_SUCCESS;
        break; //a na zbytek dlabeme
      } else if ((strcmp(argv[pct], "-l") == 0) || (strcmp(argv[pct], "--list") == 0)) {
        printf("Available input ports:\n");
        system("aconnect -i");
        printf("\n\n");
      } else if ((strcmp(argv[pct], "-p") == 0) || (strcmp(argv[pct], "--port") == 0)) {
        portname = argv[++pct];
      } else if ((strcmp(argv[pct], "-f") == 0) || (strcmp(argv[pct], "--file") == 0)) {
        cfgfile = argv[++pct];
      } else if ((strcmp(argv[pct], "-c") == 0) || (strcmp(argv[pct], "--controller") == 0)) {
        preset.param = atoi(argv[++pct]);
      } else if ((strcmp(argv[pct], "-x") == 0) || (strcmp(argv[pct], "--channel") == 0)) {
        i = atoi(argv[++pct]); //nabereme kanal
        i &= (i - 1)&0x0F; //1-16 prevedeme na 0-15
        event_trigger = 0xB0 + i; //aktivovat budem kontrollerem na zvolenem kanale
        event_param = 0x90 + i; //predvolbu vybereme pomoci noteon na zvolenem kanale
      } else if (strcmp(argv[pct], "-v") == 0) {
        verbose = -1;
      } else if (strcmp(argv[pct], "--verbose") == 0) {
        verbose = atoi(argv[++pct]); //nabereme masku pro ukecanost
      }
      pct++;
    }
  if (portname == NULL) {
    error("no port specified, see help, --port switch");
    return EXIT_FAILURE;
  }
  if (cfgfile == NULL) {
    error("no config file, see help, --file switch");
    return EXIT_FAILURE;
  }
  read_cfg_file(cfgfile); //cteme konfiguracni soubor
  start_midish(); // spustime midish
  //rawmidi_init(); //inicalizace midi procesoru
  seqmidi_init(); //inicializace midi rozhrani
  while (ext_brk == 0) {
    //rawmidi_proc();
    seqmidi_proc();
  }
  stop_midish(); //zarazim midish
  kbd_done();
  puts("done");
  return EXIT_SUCCESS; //a oznamuju uspesny konec
}
