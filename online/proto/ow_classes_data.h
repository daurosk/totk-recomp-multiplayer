/* The class table as text, written by tools/gen_classes.py from online/proto/classes.txt: edit that file
 * and run the tool, never this file. */
#ifndef OW_CLASSES_DATA_H
#define OW_CLASSES_DATA_H

#define OW_CLASSES_TEXT \
    "# What the actors add-on shares (docs/online-wire.md, \"The actors add-on\").\n" \
    "#\n" \
    "# One row per actor-name pattern, five columns separated by spaces:\n" \
    "#\n" \
    "#   pattern   A name prefix, or *TEXT for a name that contains TEXT. A *TEXT row that matches decides before any\n" \
    "#             prefix row (a Talus's weak point is a part, whatever its body's prefix says); among rows of one kind\n" \
    "#             the longest pattern decides. A name no row matches is not shared.\n" \
    "#   class     map_object world_object enemy boss animal townsperson item projectile puppet device (OW_CLASS_*),\n" \
    "#             or local: never shared, each game keeps its own.\n" \
    "#   id        map: placed by the map, known by its map instance id. creator: made at run time, known by its\n" \
    "#             creator's account and counter. map|creator: either. leader: id-less, one game leads (the herd rule).\n" \
    "#             - for local rows.\n" \
    "#   comps     The components its states carry, from anim life gear mind ride glide aim held scale, joined by\n" \
    "#             commas; - for none.\n" \
    "#   rules     Owner rules beyond the common ones, joined by commas; - for none:\n" \
    "#               still        keeps its owner while it has not moved 1 m in 5 s (rule 5)\n" \
    "#               fight        stays with its owner while it was hit in the last 10 s (rule 7)\n" \
    "#               lock         a boss: stays with its owner while a player is near (rule 7)\n" \
    "#               part=P|Q     follows the nearest record within 60 m whose name starts with P or Q (rule 8)\n" \
    "#               held         follows the enemy within 12 m that holds or throws it (rule 8)\n" \
    "#               fixed        never changes owner (rule 8)\n" \
    "#               quest        once owned, moves only when its owner leaves or a player holds it (rule 8)\n" \
    "#               cluster      a contraption part: parts within 6 m of one another are decided as one (rule 11)\n" \
    "#               core         what makes a cluster a vehicle (rule 11)\n" \
    "#               take         a take-once thing: the first game to take it gets it (take, taken)\n" \
    "#\n" \
    "# Edit this file, then run python tools/gen_classes.py: tests/net_proto_check.py fails while ow_classes_data.h\n" \
    "# differs. The rows come from the old port's sharing table and owner rules; none has been shown on two screens of this\n" \
    "# port yet, and a row that misbehaves in a round is made local until it is fixed.\n" \
    "\n" \
    "# pattern                          class         id           comps               rules\n" \
    "# -- players' own things: never actors of the add-on\n" \
    "Armor_                             local         -            -                   -\n" \
    "PlayerPuppet                       local         -            -                   -\n" \
    "DgnObj_                            local         -            -                   -\n" \
    "*Arrow                             local         -            -                   -\n" \
    "\n" \
    "# -- enemies and bosses\n" \
    "Enemy_                             enemy         map|creator  anim,life,mind,gear fight\n" \
    "Enemy_Giant_                       boss          map|creator  anim,life,mind      lock\n" \
    "Enemy_Giant_Bone                   boss          map|creator  anim,life,mind      lock\n" \
    "Enemy_Golem_                       boss          map|creator  anim,life,mind      lock\n" \
    "Enemy_Lynel_                       boss          map|creator  anim,life,mind,gear lock\n" \
    "Enemy_Zonau_BlockMaster            boss          map|creator  anim,life,mind      lock\n" \
    "Enemy_DarkLinkBoss                 boss          map|creator  anim,life,mind,gear lock\n" \
    "Zonau_BlockMaster_                 enemy         map|creator  life                part=Enemy_Zonau_BlockMaster\n" \
    "Enemy_Giant_Bone_Arm_              enemy         map|creator  anim,life           part=Enemy_Giant_Bone\n" \
    "Enemy_Giant_Bone_Eye               enemy         map|creator  anim,life           part=Enemy_Giant_Bone\n" \
    "Enemy_Giant_Bone_Chin              enemy         map|creator  anim,life           part=Enemy_Giant_Bone\n" \
    "Enemy_Giant_Bone_Rib               enemy         map|creator  anim,life           part=Enemy_Giant_Bone\n" \
    "*WeakPoint                         enemy         map|creator  life                part=Enemy_\n" \
    "*Bullet                            projectile    creator      -                   fixed\n" \
    "Enemy_EventStarter                 local         -            -                   -\n" \
    "Enemy_Army_Manager                 local         -            -                   -\n" \
    "\n" \
    "# -- townspeople\n" \
    "Npc_                               townsperson   map          anim,mind           -\n" \
    "Npc_Goron_Yunbo_Sage_Puppet        townsperson   map          anim,life,mind      quest\n" \
    "Npc_RaumiGolem_Sage                townsperson   map          anim,life,mind,ride quest\n" \
    "\n" \
    "# -- animals\n" \
    "Animal_                            animal        map|leader   anim,life           -\n" \
    "Animal_Fish_                       animal        map          anim                -\n" \
    "Animal_Horse_                      animal        map          anim,life,ride      -\n" \
    "Animal_Insect_                     animal        leader       -                   take\n" \
    "Item_FishGet_                      animal        leader       -                   take\n" \
    "\n" \
    "# -- items and gear lying in the world\n" \
    "Item_                              item          map|creator  -                   still,take\n" \
    "Weapon_                            item          map|creator  -                   still,take\n" \
    "\n" \
    "# -- world objects\n" \
    "Obj_                               world_object  map          -                   -\n" \
    "Obj_Tree                           map_object    map          life                still\n" \
    "Obj_Plant                          map_object    map          life                still\n" \
    "FldObj_Pinecone                    map_object    map          -                   still\n" \
    "FldObj_BirdNest                    map_object    map          -                   still\n" \
    "Obj_LiftRock                       world_object  map|creator  -                   held\n" \
    "Obj_RockBroken_Miasma_A_06         world_object  map          life                quest\n" \
    "Barrel                             world_object  map|creator  life                held\n" \
    "Kibako                             world_object  map|creator  life                held\n" \
    "\n" \
    "# -- Zonai devices and building parts\n" \
    "SpObj_                             device        map|creator  -                   cluster,core\n" \
    "AsbObj_                            world_object  map|creator  -                   cluster,held\n"

#endif
