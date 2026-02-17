# In a directory with svgs:
# ls | grep svg | cut -f 1 -d '.' | xargs -i echo \"{}\" | wl-copy

set(UI5_SVG_FILES
    "delete"
    "refresh"
    "question-mark"
    "laptop"
    "ipad"
    "iphone"
    "account"
    "accessibility"
    "area-chart"
    "favorite"
    "fax-machine"
    "flag"
    "family-care"
    "home"
    "home-share"
    "heart"
    "heart-2"
    "key"
    "feed"
    "it-instance"
    "it-host"
    "it-system"
    "lab"
    "machine"
    "meal"
    "physical-activity"
    "primary-key"
    "shipping-status"
    "shield"
    "study-leave"
    "subway-train"
    "syringe"
    "tag"
    "web-cam"
    "sound-loud"
    "simple-payment"
    "print"
    "nutrition-activity"
    "lightbulb"
    "decline"
    "settings"
    "add"
    "wrench"
    "navigation-right-arrow"
    "navigation-down-arrow"
    "action-settings"
    "activity-items"
    "zoom-in"
    "information"
    "alert"
    "error"
    "incident"
    "hide"
)

set(UI5_SVG_DIR "node_modules/@ui5/webcomponents-icons/dist/v5")
set(UI5_SVGS_FULLPATH "")

foreach(name ${UI5_SVG_FILES})
    set(svg_file "${UI5_SVG_DIR}/${name}.svg")
    message(STATUS "Adding SVG file: ${svg_file}")
    list(APPEND UI5_SVGS_FULLPATH ${svg_file})
endforeach()