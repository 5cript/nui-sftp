#include <frontend/icon_from_name.hpp>

#include <frontend/svgs/laptop.hpp>
#include <frontend/svgs/ipad.hpp>
#include <frontend/svgs/iphone.hpp>
#include <frontend/svgs/account.hpp>
#include <frontend/svgs/accessibility.hpp>
#include <frontend/svgs/area-chart.hpp>
#include <frontend/svgs/favorite.hpp>
#include <frontend/svgs/fax-machine.hpp>
#include <frontend/svgs/flag.hpp>
#include <frontend/svgs/family-care.hpp>
#include <frontend/svgs/home.hpp>
#include <frontend/svgs/home-share.hpp>
#include <frontend/svgs/heart.hpp>
#include <frontend/svgs/heart-2.hpp>
#include <frontend/svgs/key.hpp>
#include <frontend/svgs/feed.hpp>
#include <frontend/svgs/it-instance.hpp>
#include <frontend/svgs/it-host.hpp>
#include <frontend/svgs/it-system.hpp>
#include <frontend/svgs/lab.hpp>
#include <frontend/svgs/machine.hpp>
#include <frontend/svgs/meal.hpp>
#include <frontend/svgs/physical-activity.hpp>
#include <frontend/svgs/primary-key.hpp>
#include <frontend/svgs/shipping-status.hpp>
#include <frontend/svgs/shield.hpp>
#include <frontend/svgs/study-leave.hpp>
#include <frontend/svgs/subway-train.hpp>
#include <frontend/svgs/syringe.hpp>
#include <frontend/svgs/tag.hpp>
#include <frontend/svgs/web-cam.hpp>
#include <frontend/svgs/sound-loud.hpp>
#include <frontend/svgs/simple-payment.hpp>
#include <frontend/svgs/print.hpp>
#include <frontend/svgs/nutrition-activity.hpp>
#include <frontend/svgs/lightbulb.hpp>

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