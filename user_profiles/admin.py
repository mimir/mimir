from django.contrib import admin
from user_profiles.models import UserProfile, UserTakesLesson, UserAnswersQuestion

admin.site.register(UserProfile)
admin.site.register(UserAnswersQuestion)
admin.site.register(UserTakesLesson)
