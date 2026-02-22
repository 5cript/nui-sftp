# In a directory with svgs:
# ls | grep svg | cut -f 1 -d '.' | xargs -i echo \"{}\" | wl-copy

set(UI5_SVG_FILES
    "accessibility"
    "account"
    "action-settings"
    "activity-items"
    "add"
    "alert"
    "area-chart"
    "decline"
    "delete"
    "error"
    "family-care"
    "favorite"
    "fax-machine"
    "feed"
    "flag"
    "heart"
    "heart-2"
    "hide"
    "home"
    "home-share"
    "incident"
    "information"
    "ipad"
    "iphone"
    "it-host"
    "it-instance"
    "it-system"
    "key"
    "lab"
    "laptop"
    "lightbulb"
    "machine"
    "meal"
    "navigation-down-arrow"
    "navigation-right-arrow"
    "nutrition-activity"
    "physical-activity"
    "primary-key"
    "print"
    "question-mark"
    "refresh"
    "settings"
    "shield"
    "shipping-status"
    "simple-payment"
    "sound-loud"
    "study-leave"
    "subway-train"
    "syringe"
    "tag"
    "web-cam"
    "wrench"
    "zoom-in"
)

set(UI5_SVG_DIR "${CMAKE_BINARY_DIR}/node_modules/@ui5/webcomponents-icons/dist/v5")
set(UI5_SVGS_FULLPATH "")

foreach(name ${UI5_SVG_FILES})
    set(svg_file "${UI5_SVG_DIR}/${name}.svg")
    list(APPEND UI5_SVGS_FULLPATH ${svg_file})
endforeach()