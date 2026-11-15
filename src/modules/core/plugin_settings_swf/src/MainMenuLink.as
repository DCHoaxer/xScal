package
{
   import flash.events.Event;
   import flash.utils.getDefinitionByName;

   // Adds the PLUGINS SETTINGS entry to the main list of seventysixmenu.swf and reports when it is
   // accepted. The game replaces MenuListData on native updates, so update() inserts the entry again
   // whenever it is missing. Also adds the X CREDITS hint to the main list's button hints, next to the
   // game's $NEWS and $CHANGEUSER.
   public final class MainMenuLink
   {

      private static const ENTRY_KEYWORD:String = "xscal_plugin_settings";

      // An action the game does not know: its accept handler plays the OK sound and does nothing else.
      private static const ENTRY_ACTION:String = "xScalPluginSettings";

      private static const SETTINGS_KEYWORD:String = "SettingsMenuListData";

      private static const GALLERY_KEYWORD:String = "WorldData::OpenPhotoGallery";

      // At most this many rows are added to the main list's own row count.
      private static const MAX_ADDED_ROWS:int = 4;

      private var onAccepted:Function;

      // numItems_Inspectable of the main list before it was raised, -1 until read.
      private var baseRowCount:int = -1;

      private var onCreditsRequested:Function;

      private var creditsHint:* = null;

      private var creditsHintText:String = "";

      private var listenedMainList:* = null;

      private var entryText:String = "";

      public function MainMenuLink(acceptedCallback:Function, creditsCallback:Function)
      {
         super();
         this.onAccepted = acceptedCallback;
         this.onCreditsRequested = creditsCallback;
      }

      public function setCreditsHintText(text:String) : void
      {
         this.creditsHintText = text;
         if(this.creditsHint != null)
         {
            this.creditsHint.ButtonText = text;
         }
      }

      public function setEntryText(text:String) : void
      {
         this.entryText = text;
         if(this.listenedMainList == null)
         {
            return;
         }
         try
         {
            for each(var item:* in this.listenedMainList.MenuList_mc.List_mc.MenuListData)
            {
               if(item != null && item.dataKeyword == ENTRY_KEYWORD)
               {
                  item.text = text;
                  this.listenedMainList.MenuList_mc.SetIsDirty();
               }
            }
         }
         catch(failure:Error)
         {
         }
      }

      public function update(root1:*) : void
      {
         try
         {
            var mainList:* = root1.MainList;
            if(mainList == null)
            {
               return;
            }
            if(mainList !== this.listenedMainList)
            {
               mainList.addEventListener("MenuAccept",this.onMenuAccept);
               this.listenedMainList = mainList;
            }
            this.insertEntry(mainList);
            this.fitRowCount(mainList);
            this.addCreditsHint(mainList);
         }
         catch(failure:Error)
         {
         }
      }

      // The main list shows numItems_Inspectable rows (a component parameter in the SWF) and scrolls the
      // rest. Entries added by xScal and plugins (PLUGINS SETTINGS, a world browser) pushed Quit out of view,
      // so the row count is raised to the entry count, by at most MAX_ADDED_ROWS. A count of 0 means the
      // game already sizes the list to its entries.
      private function fitRowCount(mainList:*) : void
      {
         var menuList:* = mainList.MenuList_mc;
         var data:* = menuList.List_mc.MenuListData;
         if(data == null)
         {
            return;
         }
         var current:int = int(menuList.numItems_Inspectable);
         if(this.baseRowCount < 0)
         {
            this.baseRowCount = current;
         }
         if(this.baseRowCount <= 0)
         {
            return;
         }
         var wanted:int = Math.max(this.baseRowCount,Math.min(int(data.length),this.baseRowCount + MAX_ADDED_ROWS));
         if(wanted != current)
         {
            menuList.numItems_Inspectable = wanted;
         }
      }

      // The game shows the main list's hints while that list is active, so the hint needs no visibility
      // handling of its own.
      private function addCreditsHint(mainList:*) : void
      {
         var hintList:* = mainList.MenuList_mc;
         if(this.creditsHint == null)
         {
            var hintClass:Class = getDefinitionByName("Shared.AS3.BSButtonHintData") as Class;
            this.creditsHint = new hintClass(this.creditsHintText,"X","PSN_X","Xenon_X",1,this.onCreditsHintClicked);
         }
         if(!hintList.HasButtonHintData(this.creditsHint))
         {
            hintList.AddButtonHintData(this.creditsHint);
            hintList.connectButtonBar();
         }
      }

      private function onCreditsHintClicked() : void
      {
         this.onCreditsRequested();
      }

      private function onMenuAccept(event:Event) : void
      {
         if(event["Data"] == ENTRY_KEYWORD)
         {
            this.onAccepted();
         }
      }

      // The entry goes right after the game's Settings. A list without Settings gets it in front of
      // Photo Gallery, and a list with neither (a pause menu state, for example) gets no entry.
      private function insertEntry(mainList:*) : void
      {
         var list:* = mainList.MenuList_mc.List_mc;
         var data:* = list.MenuListData;
         if(data == null)
         {
            return;
         }
         var count:int = int(data.length);
         var settingsIndex:int = -1;
         var galleryIndex:int = -1;
         var i:int;
         for(i = 0; i < count; i++)
         {
            var item:* = data[i];
            if(item == null)
            {
               continue;
            }
            if(item.dataKeyword == ENTRY_KEYWORD)
            {
               return;
            }
            if(item.dataKeyword == SETTINGS_KEYWORD)
            {
               settingsIndex = i;
            }
            else if(item.dataKeyword == GALLERY_KEYWORD)
            {
               galleryIndex = i;
            }
         }
         var anchorIndex:int = settingsIndex >= 0 ? settingsIndex : galleryIndex;
         if(anchorIndex < 0)
         {
            return;
         }
         var insertIndex:int = settingsIndex >= 0 ? settingsIndex + 1 : galleryIndex;
         var anchor:* = data[anchorIndex];
         var entry:Object = {"text":this.entryText,"action":ENTRY_ACTION,"dataKeyword":ENTRY_KEYWORD,"disabled":false};
         // MainMenuEntry switches its frame by these two.
         if(anchor.entryStyle != null)
         {
            entry.entryStyle = anchor.entryStyle;
         }
         if(anchor.image != null)
         {
            entry.image = anchor.image;
         }
         var rebuilt:Array = [];
         for(i = 0; i < count; i++)
         {
            if(i == insertIndex)
            {
               rebuilt.push(entry);
            }
            rebuilt.push(data[i]);
         }
         if(insertIndex >= count)
         {
            rebuilt.push(entry);
         }
         var selected:int = int(list.selectedIndex);
         list.MenuListData = rebuilt;
         mainList.MenuList_mc.SetIsDirty();
         if(selected >= 0)
         {
            list.selectedIndex = selected >= insertIndex ? selected + 1 : selected;
         }
      }
   }
}
