#pragma once

#define DEFINE_EXCEPTION(name, base) \
  class name : public base \
  { public: name (const char *what); };

#define IMPL_EXCEPTION(name, base) \
  name :: name (const char *what) : base (what) {}
