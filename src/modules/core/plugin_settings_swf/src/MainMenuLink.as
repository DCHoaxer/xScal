package
{
   import flash.events.Event;

   // Adds the PLUGINS SETTINGS entry to the main list of seventysixmenu.swf and reports when it is
   // accepted. The game replaces MenuListData on native updates, so update() inserts the entry again
   // whenever it is missing.
   public final class MainMenuLink
   {

      private static const ENTRY_KEYWORD:String = "xscal_plugin_settings";

      // An action the game does not know: its accept handler plays the OK sound and does nothing else.
      private static const ENTRY_ACTION:String = "xScalPluginSettings";

      private static const SETTINGS_KEYWORD:String = "SettingsMenuListData";

      private static const GALLERY_KEYWORD:String = "WorldData::OpenPhotoGallery";

      private var onAccepted:Function;

      private var listenedMainList:* = null;

      private var entryText:String = "";

      public function MainMenuLink(acceptedCallback:Function)
      {
         super();
         this.onAccepted = acceptedCallback;
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
         }
         catch(failure:Error)
         {
         }
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
