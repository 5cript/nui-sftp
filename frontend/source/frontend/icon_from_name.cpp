#include <frontend/icon_from_name.hpp>

#include <svgs/laptop.hpp>
#include <svgs/ipad.hpp>
#include <svgs/iphone.hpp>
#include <svgs/account.hpp>
#include <svgs/accessibility.hpp>
#include <svgs/area-chart.hpp>
#include <svgs/favorite.hpp>
#include <svgs/fax-machine.hpp>
#include <svgs/flag.hpp>
#include <svgs/family-care.hpp>
#include <svgs/home.hpp>
#include <svgs/home-share.hpp>
#include <svgs/heart.hpp>
#include <svgs/heart-2.hpp>
#include <svgs/key.hpp>
#include <svgs/feed.hpp>
#include <svgs/it-instance.hpp>
#include <svgs/it-host.hpp>
#include <svgs/it-system.hpp>
#include <svgs/lab.hpp>
#include <svgs/machine.hpp>
#include <svgs/meal.hpp>
#include <svgs/physical-activity.hpp>
#include <svgs/primary-key.hpp>
#include <svgs/shipping-status.hpp>
#include <svgs/shield.hpp>
#include <svgs/study-leave.hpp>
#include <svgs/subway-train.hpp>
#include <svgs/syringe.hpp>
#include <svgs/tag.hpp>
#include <svgs/web-cam.hpp>
#include <svgs/sound-loud.hpp>
#include <svgs/simple-payment.hpp>
#include <svgs/print.hpp>
#include <svgs/nutrition-activity.hpp>
#include <svgs/lightbulb.hpp>

Nui::ElementRenderer iconFromName(std::string const& icon)
{
    using namespace GeneratedSvgs;

    if (icon == "laptop")
        return laptop();
    if (icon == "ipad")
        return ipad();
    if (icon == "iphone")
        return iphone();
    if (icon == "account")
        return account();
    if (icon == "accessibility")
        return accessibility();
    if (icon == "area-chart")
        return areachart();
    if (icon == "favorite")
        return favorite();
    if (icon == "fax-machine")
        return faxmachine();
    if (icon == "flag")
        return flag();
    if (icon == "family-care")
        return familycare();
    if (icon == "home")
        return home();
    if (icon == "home-share")
        return homeshare();
    if (icon == "heart")
        return heart();
    if (icon == "heart-2")
        return heart2();
    if (icon == "key")
        return key();
    if (icon == "feed")
        return feed();
    if (icon == "it-instance")
        return itinstance();
    if (icon == "it-host")
        return ithost();
    if (icon == "it-system")
        return itsystem();
    if (icon == "lab")
        return lab();
    if (icon == "machine")
        return machine();
    if (icon == "meal")
        return meal();
    if (icon == "physical-activity")
        return physicalactivity();
    if (icon == "primary-key")
        return primarykey();
    if (icon == "shipping-status")
        return shippingstatus();
    if (icon == "shield")
        return shield();
    if (icon == "study-leave")
        return studyleave();
    if (icon == "subway-train")
        return subwaytrain();
    if (icon == "syringe")
        return syringe();
    if (icon == "tag")
        return tag();
    if (icon == "web-cam")
        return webcam();
    if (icon == "sound-loud")
        return soundloud();
    if (icon == "simple-payment")
        return simplepayment();
    if (icon == "print")
        return print();
    if (icon == "nutrition-activity")
        return nutritionactivity();
    if (icon == "lightbulb")
        return lightbulb();

    return Nui::nil();
}