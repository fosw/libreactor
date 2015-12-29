#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include <dynamic.h>

#include "reactor_user.h"
#include "reactor_desc.h"
#include "reactor_core.h"
#include "reactor_stream.h"

void reactor_stream_init(reactor_stream *stream, reactor_user_call *call, void *state)
{
  *stream = (reactor_stream) {.state = REACTOR_STREAM_CLOSED};
  reactor_desc_init(&stream->desc, reactor_stream_event, stream);
  reactor_stream_user(stream, call, state);
  buffer_init(&stream->input);
  buffer_init(&stream->output);
}

void reactor_stream_user(reactor_stream *stream, reactor_user_call *call, void *state)
{
  reactor_user_init(&stream->user, call, state);
}

int reactor_stream_open(reactor_stream *stream, int fd)
{
  int e;

  if (stream->state != REACTOR_STREAM_CLOSED)
    return -1;

  e = reactor_desc_open(&stream->desc, fd);
  if (e == -1)
    return -1;

  reactor_desc_events(&stream->desc, REACTOR_DESC_READ | REACTOR_DESC_WRITE);
  stream->flags |= REACTOR_STREAM_WRITE_BLOCKED;
  stream->state = REACTOR_STREAM_OPEN;
  return 0;
}

void reactor_stream_error(reactor_stream *stream)
{
  reactor_user_dispatch(&stream->user, REACTOR_STREAM_ERROR, stream);
  reactor_stream_close(stream);
}

void reactor_stream_event(void *state, int type, void *data)
{
  reactor_stream *stream;

  (void) data;
  stream = state;

  if (!type)
    {
      reactor_stream_error(stream);
      return;
    }

  if (type & REACTOR_DESC_CLOSE)
    {
      buffer_clear(&stream->input);
      buffer_clear(&stream->output);
      stream->state = REACTOR_STREAM_CLOSED;
      reactor_user_dispatch(&stream->user, REACTOR_STREAM_CLOSE, stream);
      return;
    }

  if (stream->state == REACTOR_STREAM_OPEN && type & REACTOR_DESC_READ)
    reactor_stream_read(stream);

  if (type & REACTOR_DESC_WRITE)
    {
      stream->flags &= ~REACTOR_STREAM_WRITE_BLOCKED;
      if (stream->state == REACTOR_STREAM_OPEN)
        reactor_user_dispatch(&stream->user, REACTOR_STREAM_WRITE_AVAILABLE, stream);
    }

  if (~stream->flags & REACTOR_STREAM_WRITE_BLOCKED)
    reactor_stream_flush(stream);
}

void reactor_stream_read(reactor_stream *stream)
{
  reactor_stream_data data;
  char buffer[REACTOR_STREAM_BUFFER_SIZE];
  ssize_t n;

  while (stream->state == REACTOR_STREAM_OPEN)
    {
      n = read(reactor_desc_fd(&stream->desc), buffer, sizeof buffer);
      if (n <= 0)
        {
          if (n == 0)
            reactor_user_dispatch(&stream->user, REACTOR_STREAM_END, &stream);
          else if (errno != EAGAIN)
            reactor_stream_error(stream);
          return;
        }

      if (buffer_size(&stream->input) == 0)
        {
          reactor_stream_data_init(&data, buffer, n);
          reactor_user_dispatch(&stream->user, REACTOR_STREAM_DATA, &data);
          buffer_insert(&stream->input, buffer_size(&stream->input), data.base, data.size);
        }
      else
        {
          buffer_insert(&stream->input, buffer_size(&stream->input), buffer, n);
          reactor_stream_data_init(&data, buffer_data(&stream->input), buffer_size(&stream->input));
          reactor_user_dispatch(&stream->user, REACTOR_STREAM_DATA, &data);
          buffer_erase(&stream->input, 0, buffer_size(&stream->input) - data.size);
        }
    }
}

int reactor_stream_write(reactor_stream *stream, char *data, size_t size)
{
  return buffer_insert(&stream->output, buffer_size(&stream->output), data, size);
}

int reactor_stream_puts(reactor_stream *stream, char *string)
{
  return reactor_stream_write(stream, string, strlen(string));
}

int reactor_stream_putu(reactor_stream *stream, uint32_t n)
{
  int e;
  uint32_t pow10[] = {0, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000, 1000000000};
  char digits[200] =
    "0001020304050607080910111213141516171819202122232425262728293031323334353637383940414243444546474849"
    "5051525354555657585960616263646566676869707172737475767778798081828384858687888990919293949596979899";
  uint32_t t, size, x;
  buffer *b;
  char *base;

  t = (32 - __builtin_clz(n | 1)) * 1233 >> 12;
  size = t - (n < pow10[t]) + 1;

  b = &stream->output;
  e = buffer_reserve(b, buffer_size(b) + size);
  if (e == -1)
    return -1;
  b->size += size;
  base = buffer_data(b) + buffer_size(b);

  while (n >= 100)
    {
      x = (n % 100) << 1;
      n /= 100;
      *--base = digits[x + 1];
      *--base = digits[x];
    }
  if (n >= 10)
    {
      x = n << 1;
      *--base = digits[x + 1];
      *--base = digits[x];
    }
  else
      *--base = n + '0';

  return 0;
}

int reactor_stream_printf(reactor_stream *stream, const char *format, ...)
{
  int e, n;
  va_list ap;

  va_start(ap, format);
  n = vsnprintf(NULL, 0, format, ap);
  va_end(ap);
  if (n < 0)
    return -1;

  e = buffer_reserve(&stream->output, buffer_size(&stream->output) + n + 1);
  if (e == -1)
    return -1;

  va_start(ap, format);
  (void) vsnprintf(buffer_data(&stream->output) + buffer_size(&stream->output), n + 1, format, ap);
  stream->output.size += n;
  va_end(ap);
  return 0;
}

void reactor_stream_flush(reactor_stream *stream)
{
  ssize_t n;
  int fd, events;

  fd = reactor_desc_fd(&stream->desc);
  while (buffer_size(&stream->output))
    {
      stream->flags &= ~REACTOR_STREAM_WRITE_BLOCKED;
      n = write(fd, buffer_data(&stream->output), buffer_size(&stream->output));
      if (n == -1)
        {
          if (errno == EAGAIN)
            {
              stream->flags |= REACTOR_STREAM_WRITE_BLOCKED;
              events = REACTOR_DESC_WRITE | REACTOR_DESC_READ;
              reactor_desc_events(&stream->desc, events);
              return;
            }

          reactor_stream_error(stream);
          return;
        }
      stream->written += n;
      buffer_erase(&stream->output, 0, n);
    }

  if (stream->state == REACTOR_STREAM_OPEN)
    reactor_desc_events(&stream->desc, REACTOR_DESC_READ);

  if (stream->state == REACTOR_STREAM_LINGER)
    {
      stream->state = REACTOR_STREAM_CLOSING;
      reactor_desc_close(&stream->desc);
    }
}

void reactor_stream_close(reactor_stream *stream)
{
  if (stream->state != REACTOR_STREAM_OPEN)
    return;

  if (buffer_size(&stream->output))
    {
      stream->state = REACTOR_STREAM_LINGER;
      reactor_desc_events(&stream->desc, REACTOR_DESC_WRITE);
    }
  else
    {
      stream->state = REACTOR_STREAM_CLOSING;
      reactor_desc_close(&stream->desc);
    }
}

void reactor_stream_data_init(reactor_stream_data *data, char *base, size_t size)
{
  *data = (reactor_stream_data) {.base = base, .size = size};
}

void reactor_stream_data_consume(reactor_stream_data *data, size_t size)
{
  data->base += size;
  data->size -= size;
}
