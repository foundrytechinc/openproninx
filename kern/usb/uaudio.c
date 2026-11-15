// UAUDIO

#include "inc/abi.h"
#include "inc/stat.h"
#include "defs.h"
#include "devfs.h"
#include "file.h"
#include "malloc.h"
#include "param.h"
#include "proc.h"
#include "sleeplock.h"
#include "mutex.h"
#include "usbdi.h"
#include "uio.h"

#define UAUDIO_RING 65536

struct uaudio_stream {
  int present;
  int in;
  int iface;
  int alt;
  struct usb_pipe pipe;
  const struct usb_endpoint_descriptor *ed;
  const struct uaudio_format_type_i *ft;
  int rate;
  int channels;
  int width;
  int framesize;
  int seglen;
  int running;
  int idle;
  uint8_t *ring;
  uint head; // written here
  uint tail; // taken here
  int starved; // intervals sent as silence
  int overrun;
  uint intervals;
};

struct uaudio_softc {
  struct uaudio_softc *next;
  int refs;
  int gone;
  struct usb_device *dev;
  struct uaudio_stream play;
  struct uaudio_stream record;
  uint8_t play_ring[UAUDIO_RING];
  uint8_t record_ring[UAUDIO_RING];
  int unit;
  char name[16]; // its node: audioN
  int opens;
  struct mutex lock;
  struct sleeplock stream;
};

static struct {
  struct mutex lock;
  struct uaudio_softc *list;
  struct unrhdr units;
} ua;

static struct uaudio_softc *uaudio_get(int unit) {
  struct uaudio_softc *sc;

  mtx_enter(&ua.lock);
  for (sc = ua.list; sc != 0; sc = sc->next)
    if (sc->unit == unit && !sc->gone) {
      sc->refs++;
      break;
    }
  mtx_leave(&ua.lock);
  return sc;
}

static void uaudio_rele(struct uaudio_softc *sc) {
  struct uaudio_softc **pp;

  mtx_enter(&ua.lock);
  if (--sc->refs > 0) {
    mtx_leave(&ua.lock);
    return;
  }
  for (pp = &ua.list; *pp != 0; pp = &(*pp)->next)
    if (*pp == sc) {
      *pp = sc->next;
      break;
    }
  mtx_leave(&ua.lock);
  free_unr(&ua.units, sc->unit);
  free(sc, M_DEVBUF);
}

#define UAUDIO_IDLE_INTERVALS 250

static void set_rate(struct usb_device *dev, struct uaudio_stream *st);
static uint32_t freq_of(const uint8_t *p);

static uint ring_used(struct uaudio_stream *st) { return st->head - st->tail; }

// THE STREAMS

static void play_done(struct usb_pipe *pipe, void *arg, int status, int len) {
  struct uaudio_softc *sc = arg;
  struct uaudio_stream *st = &sc->play;
  uint8_t *seg = usbd_isoc_seg(pipe, pipe->seg);
  uint have, take, first;

  (void)status;
  (void)len;
  mtx_enter(&sc->lock);
  st->intervals++;
  have = ring_used(st);
  take = have < (uint)pipe->seglen ? have : (uint)pipe->seglen;
  first = UAUDIO_RING - (st->tail % UAUDIO_RING);
  if (first > take)
    first = take;
  memmove(seg, st->ring + (st->tail % UAUDIO_RING), first);
  if (take > first)
    memmove(seg + first, st->ring, take - first);
  st->tail += take;
  if (take < (uint)pipe->seglen) {
    memset(seg + take, 0, (uint)pipe->seglen - take);
    st->starved++;
    if (++st->idle == UAUDIO_IDLE_INTERVALS + 1)
      usbd_needs_service();
  } else {
    st->idle = 0;
  }
  mtx_leave(&sc->lock);
  wakeup(&st->tail);
}

static void record_done(struct usb_pipe *pipe, void *arg, int status, int len) {
  struct uaudio_softc *sc = arg;
  struct uaudio_stream *st = &sc->record;
  const uint8_t *seg = usbd_isoc_seg(pipe, pipe->seg);
  uint room, take, first;

  if (status == USBD_STALLED || len <= 0 || len > pipe->seglen)
    return;
  len -= len % st->framesize;
  mtx_enter(&sc->lock);
  st->intervals++;
  room = UAUDIO_RING - ring_used(st);
  take = (uint)len < room ? (uint)len : room;
  if (take < (uint)len) {
    st->overrun++;
    if (++st->idle == UAUDIO_IDLE_INTERVALS + 1)
      usbd_needs_service();
  } else {
    st->idle = 0;
  }
  first = UAUDIO_RING - (st->head % UAUDIO_RING);
  if (first > take)
    first = take;
  memmove(st->ring + (st->head % UAUDIO_RING), seg, first);
  if (take > first)
    memmove(st->ring, seg + first, take - first);
  st->head += take;
  mtx_leave(&sc->lock);
  wakeup(&st->head);
}

static int stream_start(struct uaudio_softc *sc, struct uaudio_stream *st) {
  int seglen;

  if (st->running)
    return 0;
  if (!st->present)
    return -1;
  if (usbd_set_interface(sc->dev, st->iface, st->alt) < 0)
    return -1;
  set_rate(sc->dev, st);
  if (usbd_open_pipe(sc->dev, st->ed, &st->pipe) < 0)
    return -1;

  seglen = st->in ? st->pipe.maxp : st->rate * st->framesize / 1000;
  if (seglen > st->pipe.maxp)
    seglen = st->pipe.maxp;
  if (!st->in)
    seglen -= seglen % st->framesize;
  if (seglen <= 0 || seglen > USB_ISOC_STRIDE) {
    usbd_close_pipe(&st->pipe);
    return -1;
  }
  if (usbd_isoc_start(&st->pipe, seglen, USB_ISOC_SEGS,
                      st->in ? record_done : play_done, sc) < 0) {
    usbd_close_pipe(&st->pipe);
    return -1;
  }
  st->seglen = seglen;
  st->idle = 0;
  st->running = 1;
  return 0;
}

static void stream_stop(struct uaudio_softc *sc, struct uaudio_stream *st) {
  if (!st->running)
    return;
  st->running = 0;
  usbd_close_pipe(&st->pipe);
  usbd_set_interface(sc->dev, st->iface, 0);
}

static void uaudio_service(struct usb_device *dev, void *softc) {
  struct uaudio_softc *sc = softc;

  (void)dev;
  if (!sc->play.running && !sc->record.running)
    return;
  acquiresleep(&sc->stream);
  if (sc->play.running && sc->play.idle > UAUDIO_IDLE_INTERVALS &&
      ring_used(&sc->play) == 0)
    stream_stop(sc, &sc->play);
  if (sc->record.running && sc->record.idle > UAUDIO_IDLE_INTERVALS) {
    stream_stop(sc, &sc->record);
    mtx_enter(&sc->lock);
    sc->record.head = sc->record.tail = 0;
    mtx_leave(&sc->lock);
  }
  releasesleep(&sc->stream);
}

static int write_bytes(struct uaudio_softc *sc, struct inode *ip, char *src,
                       int n) {
  struct uaudio_stream *st;
  int done = 0;
  uint room, take, first;

  if (!sc->play.present)
    return -FNU_ERR_NODEVICE;
  if (n < 0)
    return -FNU_ERR_INVALID;
  st = &sc->play;
  iunlock(ip);
  acquiresleep(&sc->stream);
  if (stream_start(sc, st) < 0) {
    releasesleep(&sc->stream);
    ilock(ip);
    return -FNU_ERR_IO;
  }
  releasesleep(&sc->stream);
  mtx_enter(&sc->lock);
  while (done < n) {
    room = UAUDIO_RING - ring_used(st);
    if (room == 0) {
      if (proc_interrupted() || sc->gone) {
        mtx_leave(&sc->lock);
        ilock(ip);
        return done > 0 ? done : sc->gone ? -FNU_ERR_IO : -FNU_ERR_INTERRUPTED;
      }
      msleep(&st->tail, &sc->lock, PRIBIO | PCATCH, "uaudio", 0);
      continue;
    }
    take = (uint)(n - done) < room ? (uint)(n - done) : room;
    first = UAUDIO_RING - (st->head % UAUDIO_RING);
    if (first > take)
      first = take;
    memmove(st->ring + (st->head % UAUDIO_RING), src + done, first);
    if (take > first)
      memmove(st->ring, src + done + first, take - first);
    st->head += take;
    done += (int)take;
  }
  mtx_leave(&sc->lock);
  ilock(ip);
  return done;
}

static int read_bytes(struct uaudio_softc *sc, struct inode *ip, char *dst,
                      int n) {
  struct uaudio_stream *st;
  uint have, take, first;

  if (!sc->record.present)
    return -FNU_ERR_NODEVICE;
  if (n < 0)
    return -FNU_ERR_INVALID;
  st = &sc->record;
  iunlock(ip);
  acquiresleep(&sc->stream);
  if (stream_start(sc, st) < 0) {
    releasesleep(&sc->stream);
    ilock(ip);
    return -FNU_ERR_IO;
  }
  releasesleep(&sc->stream);
  mtx_enter(&sc->lock);
  st->idle = 0;
  while ((have = ring_used(st)) == 0) {
    if (!st->running || proc_interrupted()) {
      mtx_leave(&sc->lock);
      ilock(ip);
      return st->running ? -FNU_ERR_INTERRUPTED : -FNU_ERR_IO;
    }
    msleep(&st->head, &sc->lock, PRIBIO | PCATCH, "uaudio", 0);
  }
  take = (uint)n < have ? (uint)n : have;
  take -= take % st->framesize;
  first = UAUDIO_RING - (st->tail % UAUDIO_RING);
  if (first > take)
    first = take;
  memmove(dst, st->ring + (st->tail % UAUDIO_RING), first);
  if (take > first)
    memmove(dst + first, st->ring, take - first);
  st->tail += take;
  mtx_leave(&sc->lock);
  ilock(ip);
  return (int)take;
}


// lock held
static int uaudio_uio(struct inode *ip, struct uio *uio,
                      int (*bytes)(struct uaudio_softc *, struct inode *, char *, int)) {
  struct uaudio_softc *sc;
  char *buf;
  int got = 0, r = 0;

  if ((sc = uaudio_get(ip->minor)) == 0)
    return -FNU_ERR_NODEVICE;
  if ((buf = page_alloc()) == 0) {
    uaudio_rele(sc);
    return -FNU_ERR_NOMEMORY;
  }
  while (uio->uio_resid > 0) {
    size_t chunk = uio->uio_resid > PGSIZE ? PGSIZE : uio->uio_resid;

    if (uio->uio_rw == UIO_WRITE) {
      if (uiomove(buf, chunk, uio) != 0) {
        r = -FNU_ERR_FAULT;
        break;
      }
      r = bytes(sc, ip, buf, (int)chunk);
    } else {
      r = bytes(sc, ip, buf, (int)chunk);
      if (r > 0 && uiomove(buf, (size_t)r, uio) != 0)
        r = -FNU_ERR_FAULT;
    }
    if (r < 0)
      break;
    got += r;
    if (r < (int)chunk)
      break;
  }
  page_free(buf);
  uaudio_rele(sc);
  return got > 0 ? got : r < 0 ? r : 0;
}

static int uaudioread(struct inode *ip, struct uio *uio) {
  return uaudio_uio(ip, uio, read_bytes);
}

static int uaudiowrite(struct inode *ip, struct uio *uio) {
  return uaudio_uio(ip, uio, write_bytes);
}

static int uaudioopen(struct inode *ip, int omode) {
  struct uaudio_softc *sc = uaudio_get(ip->minor);

  (void)omode;
  if (sc == 0)
    return -1;
  mtx_enter(&sc->lock);
  sc->opens++;
  mtx_leave(&sc->lock);
  uaudio_rele(sc);
  return 0;
}

static void uaudioclose(struct inode *ip) {
  struct uaudio_softc *sc = uaudio_get(ip->minor);
  int last;

  if (sc == 0)
    return;
  mtx_enter(&sc->lock);
  last = --sc->opens <= 0;
  if (last)
    sc->opens = 0;
  mtx_leave(&sc->lock);
  if (!last) {
    uaudio_rele(sc);
    return;
  }
  acquiresleep(&sc->stream);
  mtx_enter(&sc->lock);
  while (sc->play.running && ring_used(&sc->play) > 0 && !proc_interrupted())
    msleep(&sc->play.tail, &sc->lock, PRIBIO | PCATCH, "uaudio", 0);
  mtx_leave(&sc->lock);
  stream_stop(sc, &sc->play);
  stream_stop(sc, &sc->record);
  mtx_enter(&sc->lock);
  sc->record.head = sc->record.tail = 0;
  sc->play.head = sc->play.tail = 0;
  mtx_leave(&sc->lock);
  releasesleep(&sc->stream);
  uaudio_rele(sc);
}

static int rate_listed(const struct uaudio_stream *st, uint32_t rate) {
  const struct uaudio_format_type_i *ft = st->ft;
  const uint8_t *f = ft->tSamFreq;
  int k;

  if (ft->bSamFreqType == 0)
    return ft->bLength >= 14 && rate >= freq_of(f) && rate <= freq_of(f + 3);
  for (k = 0; k < ft->bSamFreqType && k * 3 + 3 <= ft->bLength - 8; k++)
    if (freq_of(f + k * 3) == rate)
      return 1;
  return 0;
}

static void swpar_of(struct uaudio_softc *sc, struct fnu_audio_swpar *p) {
  const struct uaudio_stream *st = sc->play.present ? &sc->play : &sc->record;

  memset(p, 0, sizeof(*p));
  p->sig = 1;
  p->le = 1;
  p->bits = (uint32)st->width * 8;
  p->bps = (uint32)st->width;
  p->msb = 1;
  p->rate = (uint32)st->rate;
  p->pchan = sc->play.present ? (uint32)sc->play.channels : 0;
  p->rchan = sc->record.present ? (uint32)sc->record.channels : 0;
  p->round = st->framesize != 0 ? (uint32)(st->seglen / st->framesize) : 0;
  p->nblks = p->round != 0 && st->framesize != 0
                 ? UAUDIO_RING / (uint32)st->framesize / p->round
                 : 0;
}

static void info_of(struct uaudio_softc *sc, struct fnu_audioinfo *ai) {
  memset(ai, 0, sizeof(*ai));
  mtx_enter(&sc->lock);
  ai->present = 1;
  if (sc->play.present) {
    ai->rate = (uint32)sc->play.rate;
    ai->channels = (uint32)sc->play.channels;
    ai->width = (uint32)sc->play.width;
    ai->interval = (uint32)sc->play.seglen;
    ai->queued = ring_used(&sc->play);
    ai->capacity = UAUDIO_RING;
    ai->starved = (uint32)sc->play.starved;
    ai->intervals = sc->play.intervals;
  }
  if (sc->record.present) {
    ai->rec_present = 1;
    ai->rec_rate = (uint32)sc->record.rate;
    ai->rec_channels = (uint32)sc->record.channels;
    ai->rec_width = (uint32)sc->record.width;
    ai->rec_queued = ring_used(&sc->record);
    ai->rec_overruns = (uint32)sc->record.overrun;
    ai->rec_intervals = sc->record.intervals;
  }
  mtx_leave(&sc->lock);
}

static int ioctl_sc(struct uaudio_softc *sc, uint64_t cmd, uint64_t arg);

static int uaudioioctl(struct inode *ip, uint64_t cmd, uint64_t arg) {
  struct uaudio_softc *sc = uaudio_get(ip->minor);
  struct fnu_audioinfo ai;
  int r;

  if (sc == 0)
    return FNU_ERR_NODEVICE;
  if (cmd == FNU_AUDIOGETINFO) {
    info_of(sc, &ai);
    r = copyout(curproc->p_p->pgdir, arg, &ai, sizeof(ai)) < 0 ? FNU_ERR_FAULT : 0;
  } else
    r = ioctl_sc(sc, cmd, arg);
  uaudio_rele(sc);
  return r;
}

static int ioctl_sc(struct uaudio_softc *sc, uint64_t cmd, uint64_t arg) {
  struct fnu_audio_swpar par, now;
  int r = FNU_ERR_INVALID, play, rec;

  acquiresleep(&sc->stream);
  swpar_of(sc, &now);
  if (cmd == FNU_AUDIOGETPAR) {
    releasesleep(&sc->stream);
    return copyout(curproc->p_p->pgdir, arg, &now, sizeof(now)) < 0 ? FNU_ERR_FAULT : 0;
  }
  if (cmd != FNU_AUDIOSETPAR) {
    releasesleep(&sc->stream);
    return FNU_ERR_BADREQUEST;
  }
  if (copyin(curproc->p_p->pgdir, &par, arg, sizeof(par)) < 0) {
    releasesleep(&sc->stream);
    return FNU_ERR_FAULT;
  }
  if ((par.sig != ~0u && par.sig != now.sig) ||
      (par.le != ~0u && par.le != now.le) ||
      (par.bits != ~0u && par.bits != now.bits) ||
      (par.bps != ~0u && par.bps != now.bps) ||
      (par.msb != ~0u && par.msb != now.msb) ||
      (par.pchan != ~0u && par.pchan != now.pchan) ||
      (par.rchan != ~0u && par.rchan != now.rchan)) {
    releasesleep(&sc->stream);
    return FNU_ERR_INVALID;
  }
  play = sc->play.present && rate_listed(&sc->play, par.rate);
  rec = sc->record.present && rate_listed(&sc->record, par.rate);
  if (par.rate == ~0u)
    r = 0;
  else if ((play || rec) && !(play && sc->play.running) &&
           !(rec && sc->record.running)) {
    mtx_enter(&sc->lock);
    if (play)
      sc->play.rate = (int)par.rate;
    if (rec)
      sc->record.rate = (int)par.rate;
    mtx_leave(&sc->lock);
    r = 0;
  }
  if (r == 0) {
    swpar_of(sc, &now);
    r = copyout(curproc->p_p->pgdir, arg, &now, sizeof(now)) < 0 ? FNU_ERR_FAULT : 0;
  }
  releasesleep(&sc->stream);
  return r;
}

void uaudioinit(void) {
  devsw[AUDIO].write = uaudiowrite;
  devsw[AUDIO].class = FNU_DEVCLASS_AUDIO;
  devsw[AUDIO].read = uaudioread;
  devsw[AUDIO].open = uaudioopen;
  devsw[AUDIO].close = uaudioclose;
  devsw[AUDIO].ioctl = uaudioioctl;
}

static uint32_t freq_of(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

static int find_stream(struct usb_device *dev, int iface,
                       struct uaudio_stream *st) {
  const struct usb_interface_descriptor *id;
  const struct uaudio_as_general *as;
  const struct uaudio_format_type_i *ft;
  const struct usb_endpoint_descriptor *e;
  int alt;

  for (alt = 1; alt < 16; alt++) {
    id = usbd_find_altiface(dev, iface, alt);
    if (id == 0)
      continue;
    e = usbd_find_alt_endpoint(dev, iface, alt, 0);
    if (e == 0 || UE_GET_XFERTYPE(e->bmAttributes) != UE_ISOCHRONOUS)
      continue;
    as = usbd_find_alt_class_desc(dev, iface, alt, UDESC_CS_INTERFACE, 0);
    ft = usbd_find_alt_class_desc(dev, iface, alt, UDESC_CS_INTERFACE, 1);
    if (as == 0 || ft == 0 || as->bDescriptorSubtype != UAUDIO_AS_GENERAL ||
        ft->bDescriptorSubtype != UAUDIO_FORMAT_TYPE)
      continue;
    if (as->wFormatTag != UAUDIO_FORMAT_PCM ||
        ft->bFormatType != UAUDIO_FORMAT_TYPE_I)
      continue;
    st->rate = (int)freq_of(ft->tSamFreq);
    if (ft->bSamFreqType != 0) {
      const uint8_t *f = ft->tSamFreq;
      int k, best = 0;
      for (k = 0; k < ft->bSamFreqType && k * 3 + 3 <= ft->bLength - 8; k++) {
        int r = (int)freq_of(f + k * 3);
        if (r <= 48000 && r > best)
          best = r;
      }
      if (best > 0)
        st->rate = best;
    }
    st->channels = ft->bNrChannels;
    st->width = ft->bSubframeSize;
    if (st->rate <= 0 || st->channels <= 0 || st->width <= 0)
      continue;
    st->ft = ft;
    st->framesize = st->channels * st->width;
    st->in = UE_GET_DIR(e->bEndpointAddress) != 0;
    st->iface = iface;
    st->alt = alt;
    st->ed = e;
    st->present = 1;
    return 0;
  }
  return -1;
}

static void set_rate(struct usb_device *dev, struct uaudio_stream *st) {
  const struct uaudio_ep_general *eg =
      usbd_find_alt_class_desc(dev, st->iface, st->alt, UDESC_CS_ENDPOINT, 0);
  uint8_t freq[3];

  if (eg == 0 || eg->bDescriptorSubtype != UAUDIO_EP_GENERAL)
    return;
  if (!(eg->bmAttributes & UAUDIO_FREQ_CONTROL_BIT))
    return;
  freq[0] = (uint8_t)st->rate;
  freq[1] = (uint8_t)(st->rate >> 8);
  freq[2] = (uint8_t)(st->rate >> 16);
  usbd_do_request(dev, UT_WRITE_CLASS_ENDPOINT, UR_SET_CUR,
                  UAUDIO_SAMPLING_FREQ_CONTROL << 8, st->ed->bEndpointAddress,
                  sizeof(freq), freq);
}

// ATTACH

static int uaudio_match(struct usb_attach_arg *uaa) {
  if (uaa->iface < 0 || uaa->class_code != UCLASS_AUDIO)
    return USB_MATCH_NONE;
  return uaa->subclass == USUBCLASS_AUDIOSTREAM ? USB_MATCH_IFACE_CLASS
                                                : USB_MATCH_NONE;
}

static struct uaudio_softc *uaudio_of(struct usb_device *dev) {
  struct uaudio_softc *sc;

  mtx_enter(&ua.lock);
  for (sc = ua.list; sc != 0 && (sc->dev != dev || sc->gone); sc = sc->next)
    ;
  mtx_leave(&ua.lock);
  return sc;
}

static int uaudio_attach(struct usb_attach_arg *uaa) {
  struct usb_device *dev = uaa->dev;
  struct uaudio_softc *sc = uaudio_of(dev);
  struct uaudio_stream st;
  int first = sc == 0;

  memset(&st, 0, sizeof(st));
  if (find_stream(dev, uaa->iface, &st) < 0)
    return -1;
  if (first) {
    sc = malloc(sizeof(*sc), M_DEVBUF, M_WAITOK | M_ZERO);
    mtx_init(&sc->lock, "uaudio");
    initsleeplock(&sc->stream, "uaudiostream");
    sc->dev = dev;
    sc->unit = alloc_unr(&ua.units);
    snprintf(sc->name, sizeof(sc->name), "audio%d", sc->unit);
  } else if (st.in ? sc->record.present : sc->play.present)
    return -1;
  usbd_set_interface(dev, st.iface, 0);
  mtx_enter(&sc->lock);
  if (st.in) {
    sc->record = st;
    sc->record.ring = sc->record_ring;
  } else {
    sc->play = st;
    sc->play.ring = sc->play_ring;
  }
  mtx_leave(&sc->lock);
  printf("%s: %s %d Hz, %d channels, %d bit\n", sc->name, st.in ? "record" : "play",
          st.rate, st.channels, st.width * 8);
  mtx_enter(&ua.lock);
  sc->refs++;
  if (first) {
    sc->next = ua.list;
    ua.list = sc;
  }
  mtx_leave(&ua.lock);
  uaa->softc = sc;
  if (first) {
    devfs_add(sc->name, FNU_TYPE_DEVICE, AUDIO, sc->unit, 0600);
    hotplug_attach(FNU_DEVCLASS_AUDIO, sc->name, FNU_HOTPLUG_REMOVABLE, 0);
  }
  return 0;
}

static void uaudio_detach(struct usb_device *dev, void *softc) {
  struct uaudio_softc *sc = softc;
  int first;

  (void)dev;
  mtx_enter(&ua.lock);
  first = !sc->gone;
  sc->gone = 1;
  mtx_leave(&ua.lock);
  if (first) {
    devfs_remove(sc->name);
    hotplug_detach(FNU_DEVCLASS_AUDIO, sc->name, FNU_HOTPLUG_REMOVABLE, 0);
    acquiresleep(&sc->stream);
    stream_stop(sc, &sc->play);
    stream_stop(sc, &sc->record);
    releasesleep(&sc->stream);
    mtx_enter(&sc->lock);
    sc->play.head = sc->play.tail = 0;
    sc->record.head = sc->record.tail = 0;
    wakeup(&sc->play.tail);
    wakeup(&sc->record.head);
    mtx_leave(&sc->lock);
  }
  uaudio_rele(sc);
}

static const struct usb_driver uaudio_driver = {
    .name = "uaudio",
    .match = uaudio_match,
    .attach = uaudio_attach,
    .detach = uaudio_detach,
    .service = uaudio_service,
};

void uaudio_init(void) {
  mtx_init(&ua.lock, "uaudio");
  init_unrhdr(&ua.units, "uaudio");
  usbd_register_driver(&uaudio_driver);
}
