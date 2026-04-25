#pragma once

#include <frontend/events/frontend_events.hpp>

#include <nui/frontend/element_renderer.hpp>

#include <roar/detail/pimpl_special_functions.hpp>

class Licenses
{
  public:
    explicit Licenses(FrontendEvents* events);
    ROAR_PIMPL_SPECIAL_FUNCTIONS(Licenses);

    Nui::ElementRenderer operator()();

  private:
    void loadIfNeeded();
    Nui::ElementRenderer header();
    Nui::ElementRenderer sidebar();
    Nui::ElementRenderer main();

    struct Implementation;
    std::unique_ptr<Implementation> impl_;
};
